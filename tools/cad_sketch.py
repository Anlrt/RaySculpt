"""Editable closed XY polyline for a pad."""
import math
import numpy as np
from PySide6 import QtCore as C, QtGui as G, QtWidgets as W
from PySide6.QtCore import Qt


class Canvas(W.QWidget):
    changed=C.Signal()
    def __init__(self,points):
        super().__init__()
        self.points=[list(p) for p in points]
        self.scale=5
        self.step=1.
        self.selected=-1
        self.drag=False
        self.center=np.array([0.,0.])
        self.setMinimumSize(330,280)
        self.fit()

    def fit(self):
        if self.points:
            pts=np.array(self.points)
            self.center=(pts.min(0)+pts.max(0))/2
            self.scale=min(10.,245/max(10.,*np.ptp(pts,axis=0)))
        self.update()

    def xy(self,p):
        x,y=(np.asarray(p)-self.center)*self.scale
        return C.QPointF(x+self.width()/2,self.height()/2-y)

    def coords(self,p):
        x=(p.x()-self.width()/2)/self.scale+self.center[0]
        y=(self.height()/2-p.y())/self.scale+self.center[1]
        return [round(x/self.step)*self.step,round(y/self.step)*self.step]

    def paintEvent(self,e):
        p=G.QPainter(self)
        p.fillRect(self.rect(),G.QColor("#f5f8fa"))
        grid=self.step
        while grid*self.scale<14:
            grid*=2
        for j,size in enumerate((self.width(),self.height())):
            offset=(self.width()/2-self.center[0]*self.scale,self.height()/2+self.center[1]*self.scale)[j]
            for k in range(math.floor(-offset/(grid*self.scale)),math.ceil((size-offset)/(grid*self.scale))+1):
                dist=round(offset+k*grid*self.scale)
                p.setPen(G.QColor("#9bb2bd" if k==0 else "#dfE8ec"))
                if j==0:
                    p.drawLine(dist,0,dist,self.height())
                else:
                    p.drawLine(0,dist,self.width(),dist)
        p.setRenderHint(G.QPainter.Antialiasing)
        p.setPen(G.QPen(G.QColor("#367fa0"),2))
        p.setBrush(G.QColor(106,177,201,65))
        poly=G.QPolygonF([self.xy(pt) for pt in self.points])
        if len(poly)>=3:
            p.drawPolygon(poly)
        else:
            p.drawPolyline(poly)
        for j,pt in enumerate(poly):
            p.setBrush(G.QColor("#de8b45" if j==self.selected else "#fff"))
            p.drawEllipse(pt,4,4)
            p.drawText(pt+C.QPointF(6,-5),str(j+1))

    def mousePressEvent(self,e):
        if e.button()!=Qt.LeftButton:
            return
        hit=next((i for i,p in enumerate(self.points) if (self.xy(p)-e.position()).manhattanLength()<10),None)
        if hit is None:
            self.points.append(self.coords(e.position()))
            hit=len(self.points)-1
        self.selected=hit
        self.drag=True
        self.changed.emit()
        self.update()

    def mouseMoveEvent(self,e):
        if self.drag and self.selected>=0:
            self.points[self.selected]=self.coords(e.position())
            self.changed.emit()
            self.update()

    def mouseReleaseEvent(self,e):
        self.drag=False

    def wheelEvent(self,e):
        self.scale=float(np.clip(self.scale*1.2**(e.angleDelta().y()/120),.005,1000))
        self.update()


class Sketch(W.QDialog):
    def __init__(self,parent,points):
        super().__init__(parent)
        self.setWindowTitle("Эскиз · плоскость XY")
        self.resize(690,410)
        layout=W.QVBoxLayout(self)
        main=W.QHBoxLayout()
        self.canvas=Canvas(points)
        main.addWidget(self.canvas,1)
        side=W.QVBoxLayout()
        self.table=W.QTableWidget(0,2)
        self.table.setMaximumWidth(215)
        self.table.setHorizontalHeaderLabels(["X, мм","Y, мм"])
        self.table.horizontalHeader().setSectionResizeMode(W.QHeaderView.Stretch)
        self.table.setSelectionBehavior(W.QAbstractItemView.SelectRows)
        side.addWidget(self.table)
        for name,fn in (("Добавить вершину",self.add),("Удалить вершину",self.remove),("Вписать",self.canvas.fit)):
            b=W.QPushButton(name)
            b.clicked.connect(fn)
            side.addWidget(b)
        main.addLayout(side)
        layout.addLayout(main,1)
        layout.addWidget(W.QLabel("Щелчок добавляет вершину. Перетаскивание перемещает её с шагом 1 мм. Точные координаты задаются в таблице."))
        self.buttons=W.QDialogButtonBox(W.QDialogButtonBox.Ok|W.QDialogButtonBox.Cancel)
        self.buttons.accepted.connect(self.accept)
        self.buttons.rejected.connect(self.reject)
        layout.addWidget(self.buttons)
        self.canvas.changed.connect(self.fill)
        self.table.cellChanged.connect(self.edit)
        self.table.currentCellChanged.connect(self.select)
        self.fill()

    def select(self,row,*args):
        self.canvas.selected=row
        self.canvas.update()

    def fill(self):
        self.table.blockSignals(True)
        self.table.setRowCount(len(self.canvas.points))
        for i,point in enumerate(self.canvas.points):
            for j,value in enumerate(point):
                self.table.setItem(i,j,W.QTableWidgetItem(f"{value:.8g}"))
        if self.canvas.selected>=0:
            self.table.selectRow(self.canvas.selected)
        self.table.blockSignals(False)
        self.buttons.button(W.QDialogButtonBox.Ok).setEnabled(len(self.canvas.points)>=3)
        self.canvas.update()

    def edit(self,row,col):
        try:
            value=float(self.table.item(row,col).text().replace(",","."))
            if math.isfinite(value) and abs(value)<=1e6:
                self.canvas.points[row][col]=value
        except ValueError:
            pass
        self.fill()

    def add(self):
        last=self.canvas.points[-1] if self.canvas.points else [0,0]
        self.canvas.points.append([last[0]+5,last[1]+5])
        self.canvas.selected=len(self.canvas.points)-1
        self.fill()

    def remove(self):
        i=self.canvas.selected
        if i>=0:
            self.canvas.points.pop(i)
            self.canvas.selected=min(i,len(self.canvas.points)-1)
            self.fill()
