use serde::Deserialize;
use serde_json::Value;
use sha2::{Digest, Sha256};
use std::collections::BTreeSet;
use std::error::Error;
use std::fs::File;
use std::io::{self, Read, Write};
use std::path::Path;
use zip::write::SimpleFileOptions;
use zip::{ZipArchive, ZipWriter};

const SCENE_LIMIT: u64 = 8 * 1024 * 1024;
const PROJECT_LIMIT: u64 = 1024 * 1024 * 1024;

#[derive(Deserialize)]
pub struct MaterialChange {
    object: String,
    material: String,
    line: String,
}

fn read_scene(archive: &mut ZipArchive<File>) -> Result<(Value, BTreeSet<String>), Box<dyn Error>> {
    let bytes = (0..archive.len()).try_fold(0_u64, |total, i| {
        let size = archive.by_index_raw(i)?.size();
        Ok::<_, zip::result::ZipError>(total.saturating_add(size))
    })?;
    if bytes > PROJECT_LIMIT {
        return Err("CCAD archive exceeds the project budget".into());
    }
    let scene = {
        let entry = archive.by_name("scene.json")?;
        if entry.size() > SCENE_LIMIT {
            return Err("CCAD scene exceeds the project budget".into());
        }
        serde_json::from_reader::<_, Value>(entry.take(SCENE_LIMIT + 1))?
    };
    if !matches!(scene["version"].as_u64(), Some(1 | 2)) {
        return Err("Unsupported CCAD version".into());
    }
    let objects = scene["objects"]
        .as_array()
        .ok_or("CCAD objects are absent")?;
    let materials = scene["materials"]
        .as_object()
        .ok_or("CCAD materials are absent")?;
    let mut ids = BTreeSet::new();
    let mut assets = BTreeSet::new();
    for item in objects {
        let id = item["id"].as_str().ok_or("CCAD object ID is absent")?;
        if !ids.insert(id) {
            return Err("Duplicate CCAD object ID".into());
        }
        if let Some(key) = item["material"].as_str() {
            let material = materials.get(key).ok_or("Missing material card")?;
            let line = item["line"].as_str().ok_or("Missing material line")?;
            if !material["indices"][line]
                .as_f64()
                .is_some_and(|n| n > 1. && n <= 6.)
            {
                return Err("Invalid index of refraction".into());
            }
        }
        for field in ["asset", "profile", "image"] {
            if let Some(value) = item.get(field) {
                assets.insert(value.as_str().ok_or("Invalid file reference")?.to_owned());
            }
        }
    }
    if let Some(turntable) = scene.get("turntable") {
        assets.insert(
            turntable["asset"]
                .as_str()
                .ok_or("Missing turntable attachment")?
                .to_owned(),
        );
    }
    for key in &assets {
        let (digest, extension) = key.split_once('.').ok_or("Invalid CCAD attachment name")?;
        if digest.len() != 64
            || !digest
                .bytes()
                .all(|b| b.is_ascii_digit() || (b'a'..=b'f').contains(&b))
            || !(1..=8).contains(&extension.len())
            || !extension
                .bytes()
                .all(|b| b.is_ascii_lowercase() || b.is_ascii_digit())
        {
            return Err("Invalid CCAD attachment name".into());
        }
        let mut entry = archive.by_name(&format!("assets/{key}"))?;
        let mut hash = Sha256::new();
        let mut buffer = [0; 64 * 1024];
        loop {
            let length = entry.read(&mut buffer)?;
            if length == 0 {
                break;
            }
            hash.update(&buffer[..length]);
        }
        if format!("{:x}", hash.finalize()) != digest {
            return Err("CCAD attachment hash did not match".into());
        }
    }
    Ok((scene, assets))
}

pub fn inspect(input: &Path) -> Result<(), Box<dyn Error>> {
    let mut archive = ZipArchive::new(File::open(input)?)?;
    let (scene, _) = read_scene(&mut archive)?;
    serde_json::to_writer(io::stdout().lock(), &scene)?;
    Ok(())
}

pub fn save_as(input: &Path, output: &Path, change: MaterialChange) -> Result<(), Box<dyn Error>> {
    let mut archive = ZipArchive::new(File::open(input)?)?;
    let (mut scene, assets) = read_scene(&mut archive)?;
    let index = scene["materials"][&change.material]["indices"][&change.line].as_f64();
    if !index.is_some_and(|n| n > 1. && n <= 6.) {
        return Err("Unknown material or wavelength".into());
    }
    let object = scene["objects"]
        .as_array_mut()
        .ok_or("CCAD objects are absent")?
        .iter_mut()
        .find(|item| item["id"] == change.object)
        .ok_or("Unknown CAD object")?;
    if !object["material"].is_string() {
        return Err("Selected object has no material".into());
    }
    object["material"] = change.material.into();
    object["line"] = change.line.into();
    let metadata = serde_json::to_vec(&scene)?;
    if metadata.len() as u64 > SCENE_LIMIT {
        return Err("CCAD scene exceeds the project budget".into());
    }
    let parent = output
        .parent()
        .filter(|path| !path.as_os_str().is_empty())
        .unwrap_or_else(|| Path::new("."));
    let mut staged = tempfile::NamedTempFile::new_in(parent)?;
    {
        let mut writer = ZipWriter::new(staged.as_file_mut());
        writer.start_file(
            "scene.json",
            SimpleFileOptions::default().compression_method(zip::CompressionMethod::Deflated),
        )?;
        writer.write_all(&metadata)?;
        let mut total = metadata.len() as u64;
        for asset in assets {
            let entry = archive.by_name(&format!("assets/{asset}"))?;
            total += entry.size();
            if total > PROJECT_LIMIT {
                return Err("CCAD archive exceeds the project budget".into());
            }
            writer.raw_copy_file(entry)?;
        }
        writer.finish()?;
    }
    staged.as_file().sync_all()?;
    staged.persist_noclobber(output)?;
    Ok(())
}
