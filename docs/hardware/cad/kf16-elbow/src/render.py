"""Renders for the KF16 elbow: shaded iso views (VTK) and a dimensioned section (matplotlib).

    python3 render.py STL LEG OUTDIR
"""
import sys, os, math
import vtk
import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

stl, leg, out = sys.argv[1], float(sys.argv[2]), sys.argv[3]
os.makedirs(out, exist_ok=True)
name = os.path.splitext(os.path.basename(stl))[0]

rd = vtk.vtkSTLReader(); rd.SetFileName(stl); rd.Update()
nm = vtk.vtkPolyDataNormals(); nm.SetInputConnection(rd.GetOutputPort()); nm.SetFeatureAngle(40); nm.Update()


def shaded(view, fn, size=(1200, 1000), zoom=1.25):
    m = vtk.vtkPolyDataMapper(); m.SetInputConnection(nm.GetOutputPort())
    a = vtk.vtkActor(); a.SetMapper(m)
    p = a.GetProperty(); p.SetColor(0.78, 0.80, 0.83); p.SetSpecular(0.5); p.SetSpecularPower(30); p.SetDiffuse(0.8); p.SetAmbient(0.15)
    fe = vtk.vtkFeatureEdges(); fe.SetInputConnection(rd.GetOutputPort()); fe.BoundaryEdgesOff(); fe.ManifoldEdgesOff(); fe.NonManifoldEdgesOff(); fe.SetFeatureAngle(35)
    em = vtk.vtkPolyDataMapper(); em.SetInputConnection(fe.GetOutputPort()); em.ScalarVisibilityOff()
    ea = vtk.vtkActor(); ea.SetMapper(em); ea.GetProperty().SetColor(0.25, 0.27, 0.3); ea.GetProperty().SetLineWidth(1.5)
    r = vtk.vtkRenderer(); r.AddActor(a); r.AddActor(ea); r.SetBackground(1, 1, 1)
    w = vtk.vtkRenderWindow(); w.SetOffScreenRendering(1); w.AddRenderer(r); w.SetSize(*size); w.SetMultiSamples(8)
    c = r.GetActiveCamera()
    ctr = (leg / 2, 0, leg / 2)
    c.SetFocalPoint(*ctr); c.SetPosition(*(ctr[i] + view[0][i] * 300 for i in range(3))); c.SetViewUp(*view[1])
    r.ResetCamera(); c.Zoom(zoom)
    w.Render()
    f = vtk.vtkWindowToImageFilter(); f.SetInput(w); f.Update()
    pw = vtk.vtkPNGWriter(); pw.SetFileName(os.path.join(out, fn)); pw.SetInputConnection(f.GetOutputPort()); pw.Write()


shaded(((1.0, -1.3, 0.8), (0, 0, 1)), name + "-iso.png")
shaded(((-0.9, -1.2, -0.6), (0, 0, 1)), name + "-iso-back.png", zoom=0.95)

# section at Y = 0 with dimensions
pl = vtk.vtkPlane(); pl.SetOrigin(0, 0, 0); pl.SetNormal(0, 1, 0)
cu = vtk.vtkCutter(); cu.SetInputConnection(rd.GetOutputPort()); cu.SetCutFunction(pl); cu.Update()
pd = cu.GetOutput()
pts = np.array([pd.GetPoint(i) for i in range(pd.GetNumberOfPoints())])
fig, ax = plt.subplots(figsize=(7, 7), dpi=160)
lines = pd.GetLines(); lines.InitTraversal(); idl = vtk.vtkIdList()
while lines.GetNextCell(idl):
    ids = [idl.GetId(j) for j in range(idl.GetNumberOfIds())]
    ax.plot(pts[ids, 0], pts[ids, 2], color="0.15", lw=1.2)
ax.set_aspect("equal"); ax.axis("off")
kw = dict(arrowprops=dict(arrowstyle="<->", lw=0.8, color="#1f4e8c"))
tk = dict(color="#1f4e8c", fontsize=10, ha="center", va="center", backgroundcolor="white")
ax.plot([0, 0], [-6, leg + 4], ls="-.", lw=0.6, color="0.5"); ax.plot([-6, leg + 4], [0, 0], ls="-.", lw=0.6, color="0.5")
# leg A: centre to face along X, drawn below
y0 = -24
ax.annotate("", (0, y0), (leg, y0), **kw); ax.text(leg / 2, y0, f"{leg:g}", **tk)
ax.plot([leg, leg], [-15, y0 - 2], lw=0.5, color="#1f4e8c"); ax.plot([0, 0], [-6, y0 - 2], lw=0.5, color="#1f4e8c")
# leg B along Z, drawn left
x0 = -24
ax.annotate("", (x0, 0), (x0, leg), **kw); ax.text(x0, leg / 2, f"{leg:g}", rotation=90, **tk)
ax.plot([-15, x0 - 2], [leg, leg], lw=0.5, color="#1f4e8c"); ax.plot([-6, x0 - 2], [0, 0], lw=0.5, color="#1f4e8c")
# flange OD at port A
xa = leg + 7
ax.annotate("", (xa, -15), (xa, 15), **kw); ax.text(xa + 4, 0, "Ø30", rotation=90, **tk)
ax.plot([leg, xa + 2], [15, 15], lw=0.5, color="#1f4e8c"); ax.plot([leg, xa + 2], [-15, -15], lw=0.5, color="#1f4e8c")
ax.annotate("", (-8, leg + 6), (8, leg + 6), **kw); ax.text(0, leg + 10, "Ø16 bore", **tk)
ax.set_title(f"KF16 90° elbow, section on the centrelines, centre to face {leg:g} mm", fontsize=10)
ax.set_xlim(-34, leg + 22); ax.set_ylim(-32, leg + 16)
fig.savefig(os.path.join(out, name + "-section.png"), bbox_inches="tight", facecolor="white")
print("rendered", name)
