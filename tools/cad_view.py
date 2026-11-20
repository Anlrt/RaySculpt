"""Orthographic model viewport. Screen-space render buffers never enter the optical solver."""
import math
import os
import cv2
import numpy as np
from PySide6.QtCore import Qt, Signal, QPointF, QTimer
from PySide6.QtGui import QColor, QFont, QImage, QPainter, QPen, QPolygonF
from PySide6.QtWidgets import QApplication, QWidget
from cad_scene import SOLIDS, rotation


class View(QWidget):
    picked=Signal(str)
    dragged=Signal(str,object,bool)
    subpicked=Signal(str,object,object,float)

    def __init__(self):
        super().__init__()
        self.setMinimumSize(340,270)
        self.items=[]
        self.geometry={}
        self.consumed=set()
        self.errors=set()
        self.selected=None
        self.yaw,self.pitch=36.,26.
        self.center=np.zeros(3)
        self.scale=7.
        self.show_grid=True
        self.native=None
        self.color_relief=False
        self.press=None
        self.moving=None
        self.move_mode=False
        self.pick_mode="object"
        self.highlights=[]
        self.hit=None
        self.screen_hits=[]
        library=os.environ.get("RAYSCULPT_GPU_WIDGET")
        if library and QApplication.platformName()!="offscreen":
            try:
                from cad_view_native import NativeViewport
                self.native=NativeViewport(self,library)
            except (ImportError,OSError,RuntimeError):
                self.native=None
        self.setToolTip("ЛКМ — выбор · ПКМ — вращение · СКМ — панорама · колесо — масштаб · Shift+ЛКМ — перемещение")

    def basis(self):
        a,b=np.radians([self.yaw,self.pitch])
        return np.array([[np.cos(a),np.sin(a),0],[-np.sin(a)*np.sin(b),np.cos(a)*np.sin(b),-np.cos(b)],
                         [np.sin(a)*np.cos(b),-np.cos(a)*np.cos(b),-np.sin(b)]])

    def project(self,points):
        p=(np.asarray(points)-self.center) @ self.basis().T*self.scale
        p[...,:2]+=[self.width()/2,self.height()/2]
        return p

    def vertices(self,obj):
        if obj["id"] in self.geometry:
            points=self.geometry[obj["id"]][0]
            # Eight box corners bound even extremely fine meshes.
            a,b=points.min(axis=0),points.max(axis=0)
            points=np.array([[x,y,z] for x in (a[0],b[0]) for y in (a[1],b[1]) for z in (a[2],b[2])])
        elif obj["kind"] in ("area","screen"):
            d=obj["dims"]
            points=np.array([[-1,-1,0],[1,-1,0],[1,1,0],[-1,1,0]])*[d["w"]/2,d["d"]/2,1]
        else:
            points=np.zeros((1,3))
        return points @ rotation(obj).T+obj["pos"]

    def fit(self,all_items=False):
        objects=[i for i in self.items if all_items or i["id"]==self.selected]
        if not objects:
            return
        pts=np.concatenate([self.vertices(i) for i in objects])
        self.center=(pts.min(axis=0)+pts.max(axis=0))/2
        size=np.ptp(pts@self.basis()[:2].T,axis=0)
        self.scale=float(min(self.width()*.71/max(10,size[0]),self.height()*.71/max(10,size[1])))
        self.update()

    def update(self,*args):
        if self.native is not None:
            self.native.sync()
        super().update(*args)

    def resizeEvent(self,event):
        if self.native is not None:
            self.native.widget.setGeometry(self.rect())
        super().resizeEvent(event)

    def showEvent(self,event):
        super().showEvent(event)
        if self.native is not None:
            self.native.sync()
            QTimer.singleShot(750,self,self.check_native)

    def check_native(self):
        if self.native is not None and not self.color_relief and not self.native.ready():
            self.native.widget.hide()
            self.native.failed=True
            self.update()

    def paintEvent(self,event):
        if self.native is not None and self.native.ready():
            return
        w,h=self.width(),self.height()
        p=QPainter(self)
        p.fillRect(self.rect(),QColor("#f4f7f9"))
        p.setRenderHint(QPainter.Antialiasing)
        def line(a,b,color,width=1):
            p.setPen(QPen(QColor(color),width))
            p.drawLine(QPointF(*a[:2]),QPointF(*b[:2]))
        step=10**math.floor(math.log10(max(1e-5,75/self.scale)))
        if step*self.scale<30:
            step*=5
        if self.show_grid:
            origin=np.round(self.center[:2]/step)*step
            for k in range(-27,28):
                for axis in range(2):
                    pt=np.array([[origin[0],origin[1],0.],[origin[0],origin[1],0.]])
                    pt[:,axis]+=k*step
                    pt[:,1-axis]+=[-30*step,30*step]
                    a,b=self.project(pt)
                    line(a,b,"#c4d0d7" if k%5==0 else "#e0e7eb")
        # Rasterise display triangles sorted by camera depth; ID buffer supports picking.
        layer=np.zeros((h,w,4),np.uint8)
        self.hit=np.zeros((h,w),np.int32)
        buffers=[]
        self.screen_hits=[]
        for idx,item in enumerate(self.items,1):
            key=item["id"]
            if key not in self.geometry or item.get("hidden") or key in self.consumed:
                continue
            xyz,tri,_=self.geometry[key]
            pt=self.project(xyz @ rotation(item).T+item["pos"])
            pts=pt[tri]
            visible=(pts[:,:,0].max(1)>=0)&(pts[:,:,0].min(1)<w)&(pts[:,:,1].max(1)>=0)&(pts[:,:,1].min(1)<h)
            pts=pts[visible]
            if not len(pts):
                continue
            norms=np.cross(pts[:,1]-pts[:,0],pts[:,2]-pts[:,0])
            norms/=np.maximum(np.linalg.norm(norms,axis=1,keepdims=True),1e-15)
            lum=.67+.32*np.abs(norms @ np.array([-.35,-.55,.76]))
            base=[224,113,104] if key in self.errors else ([105,177,206] if key==self.selected else [150,170,183])
            rgb=np.uint8(np.clip(lum[:,None]*base,0,255))
            if self.color_relief and item.get("profile"):
                heights=xyz[tri[visible],2]
                top=np.min(heights,axis=1)>0
                if np.any(top):
                    z=heights[top].mean(axis=1)-item["dims"]["h"]
                    low,high=item.get("min_relief",z.min()),item.get("max_relief",z.max())
                    t=np.clip((z-low)/max(high-low,1e-9),0,1)
                    palette=np.array([[35,82,135],[76,172,179],[248,219,120]])
                    rgb[top]=np.column_stack([np.interp(t,[0,.5,1],v) for v in palette.T]).astype(np.uint8)
            buffers.append((pts,rgb,np.full(len(pts),idx)))
        if buffers:
            pts,colors,ids=[np.concatenate(parts) for parts in zip(*buffers)]
            order=np.argsort(pts[:,:,2].mean(1))[::-1]
            polygons=np.int32(np.clip(np.rint(pts[:,:,:2]),-100000,100000))
            rgba=np.column_stack((colors,np.full(len(colors),255))).tolist()
            for j in order:
                cv2.fillConvexPoly(layer,polygons[j],rgba[j])
                cv2.fillConvexPoly(self.hit,polygons[j],int(ids[j]))
            img=QImage(layer.data,w,h,w*4,QImage.Format_RGBA8888)
            p.drawImage(0,0,img)
            # Draw the outside contour without exposing display tessellation.
            for idx,item in enumerate(self.items,1):
                if item["id"]==self.selected:
                    outlines,_=cv2.findContours(np.uint8(self.hit==idx),cv2.RETR_EXTERNAL,cv2.CHAIN_APPROX_SIMPLE)
                    p.setPen(QPen(QColor("#21799c"),1.4))
                    for poly in outlines:
                        if len(poly)>2:
                            p.drawPolygon(QPolygonF([QPointF(float(x),float(y)) for x,y in poly[:,0]]))
        for idx,item in enumerate(self.items,1):
            kind=item["kind"]
            if kind in SOLIDS:
                continue
            pt=self.project(self.vertices(item))
            x,y=self.project(item["pos"])[:2]
            color="#bb4b40" if item["id"] in self.errors else ("#9b6416" if kind in ("point","area","sun") else "#407e9b")
            p.setPen(QPen(QColor(color),2 if item["id"]==self.selected else 1.4))
            if kind in ("screen","area"):
                polygon=QPolygonF([QPointF(x,y) for x,y in pt[:,:2]])
                p.setBrush(QColor(88,159,185,20) if kind=="screen" else QColor(225,181,82,45))
                p.drawPolygon(polygon)
                p.setBrush(Qt.NoBrush)
                self.screen_hits.append((idx,polygon))
            if kind in ("point","area","sun"):
                p.setBrush(QColor("#f6c363"))
                p.drawEllipse(QPointF(x,y),5,5)
                p.setBrush(Qt.NoBrush)
            if -30<=x<=w and -20<=y<=h:
                p.drawText(QPointF(x+10,y-10),item["name"])
        p.setPen(QPen(QColor("#bd5620"),2.4))
        for poly in self.highlights:
            pts=self.project(poly)
            if len(pts)==1:
                p.drawEllipse(QPointF(*pts[0,:2]),4,4)
            else:
                p.drawPolyline(QPolygonF([QPointF(*xy) for xy in pts[:,:2]]))
        p.setFont(QFont("Segoe UI",9))
        p.setPen(QColor("#526976"))
        p.drawText(18,26,"ОРТОГОНАЛЬНЫЙ ВИД")
        selected=next((i for i in self.items if i["id"]==self.selected),{})
        if self.color_relief and selected.get("profile"):
            p.drawText(18,47,f'Рельеф   {selected.get("min_relief",0):.3f} … {selected.get("max_relief",0):.3f} мм')
        base=np.array([44.,h-43])
        for (tip,col,label) in zip(self.basis()[:2].T*27,["#b75751","#4b8d68","#4c77aa"],"XYZ"):
            end=base+tip
            line(base,end,col,2)
            p.drawText(QPointF(*(end+[2,-2])),label)
        size=step*self.scale
        start=np.array([w-24-size,h-25])
        line(start,start+[size,0],"#657987",1.5)
        for shift in [0,size]:
            line(start+[shift,-3],start+[shift,3],"#657987")
        p.setPen(QColor("#526976"))
        p.drawText(QPointF(*(start+[0,-7])),f"{step:g} мм")
        p.end()

    def at(self,pos):
        if self.native is not None and self.native.ready():
            return self.native.pick(pos)
        for idx,polygon in reversed(self.screen_hits):
            if polygon.containsPoint(pos,Qt.OddEvenFill):
                return self.items[idx-1]["id"]
        for item in self.items:
            if item["kind"] in ("point","sun") and np.linalg.norm(self.project(item["pos"])[:2]-[pos.x(),pos.y()])<12:
                return item["id"]
        x,y=int(pos.x()),int(pos.y())
        if self.hit is not None and 0<=y<self.hit.shape[0] and 0<=x<self.hit.shape[1] and self.hit[y,x]:
            return self.items[self.hit[y,x]-1]["id"]
        return None

    def mousePressEvent(self,event):
        self.press=event.position()
        self.last=self.press
        if event.button()==Qt.LeftButton:
            key=self.at(self.press)
            if self.pick_mode!="object" and not self.move_mode and not event.modifiers()&Qt.ShiftModifier:
                key=key or self.selected
                if key:
                    xy=np.array([self.press.x()-self.width()/2,self.press.y()-self.height()/2])
                    origin=self.center+xy@self.basis()[:2]/self.scale
                    self.subpicked.emit(key,origin,self.basis()[2],10/self.scale)
                return
            if key:
                self.picked.emit(key)
                obj=next(i for i in self.items if i["id"]==key)
                if (self.move_mode or event.modifiers()&Qt.ShiftModifier) and not obj["locked"] and "parent" not in obj:
                    self.moving=(key,np.array(obj["pos"]))

    def mouseMoveEvent(self,event):
        if self.press is None:
            return
        delta=event.position()-self.last
        if event.buttons()&Qt.RightButton:
            self.yaw-=delta.x()*.45
            self.pitch=float(np.clip(self.pitch+delta.y()*.45,-90,90))
        elif event.buttons()&Qt.MiddleButton:
            self.center-=np.array([delta.x(),delta.y()]) @ self.basis()[:2]/self.scale
        elif self.moving is not None:
            delta=event.position()-self.press
            location=self.moving[1]+np.array([delta.x(),delta.y()]) @ self.basis()[:2]/self.scale
            self.dragged.emit(self.moving[0],location.tolist(),False)
        self.last=event.position()
        self.update()

    def mouseReleaseEvent(self,event):
        if self.moving is not None:
            delta=event.position()-self.press
            end=self.moving[1]+np.array([delta.x(),delta.y()]) @ self.basis()[:2]/self.scale
            self.dragged.emit(self.moving[0],end.tolist(),True)
        self.press=self.moving=None

    def wheelEvent(self,event):
        before=self.scale
        self.scale=float(np.clip(before*1.18**(event.angleDelta().y()/120),.002,8000))
        offset=np.array([event.position().x()-self.width()/2,event.position().y()-self.height()/2])
        self.center+=offset @ self.basis()[:2]*(1/before-1/self.scale)
        self.update()
