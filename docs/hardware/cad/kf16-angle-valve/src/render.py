"""Colour renders of the KF16 angle valve from the per-part STLs.

    python3 render.py PARTS_DIR OUTDIR
"""
import sys, os, glob, vtk

src, out = sys.argv[1], sys.argv[2]
os.makedirs(out, exist_ok=True)
COL = {"steel": (0.78, 0.80, 0.83), "alu": (0.84, 0.85, 0.87), "alu_d": (0.66, 0.68, 0.71), "black": (0.07, 0.07, 0.08),
       "black2": (0.16, 0.16, 0.17), "cable": (0.10, 0.10, 0.10), "nickel": (0.78, 0.78, 0.74), "tube": (0.25, 0.45, 0.85),
       "bronze": (0.62, 0.52, 0.35), "label": (0.86, 0.86, 0.84)}


def actors():
    out = []
    for f in sorted(glob.glob(os.path.join(src, "*.stl"))):
        c = os.path.basename(f)[:-4].split("__")[1]
        rd = vtk.vtkSTLReader(); rd.SetFileName(f)
        nm = vtk.vtkPolyDataNormals(); nm.SetInputConnection(rd.GetOutputPort()); nm.SetFeatureAngle(40)
        m = vtk.vtkPolyDataMapper(); m.SetInputConnection(nm.GetOutputPort())
        a = vtk.vtkActor(); a.SetMapper(m)
        p = a.GetProperty(); p.SetColor(*COL[c]); p.SetSpecular(0.45 if c not in ("black", "black2", "cable") else 0.25)
        p.SetSpecularPower(30); p.SetDiffuse(0.8); p.SetAmbient(0.18)
        out.append(a)
        fe = vtk.vtkFeatureEdges(); fe.SetInputConnection(rd.GetOutputPort()); fe.BoundaryEdgesOff(); fe.ManifoldEdgesOff(); fe.NonManifoldEdgesOff(); fe.SetFeatureAngle(35)
        em = vtk.vtkPolyDataMapper(); em.SetInputConnection(fe.GetOutputPort()); em.ScalarVisibilityOff()
        ea = vtk.vtkActor(); ea.SetMapper(em); ea.GetProperty().SetColor(0.25, 0.27, 0.3); ea.GetProperty().SetLineWidth(1.2)
        out.append(ea)
    return out


def shot(direction, fn, zoom=1.15, up=(0, 0, 1), size=(1100, 1300)):
    r = vtk.vtkRenderer(); r.SetBackground(1, 1, 1)
    for a in actors():
        r.AddActor(a)
    w = vtk.vtkRenderWindow(); w.SetOffScreenRendering(1); w.AddRenderer(r); w.SetSize(*size); w.SetMultiSamples(8)
    c = r.GetActiveCamera(); fp = (-5, 0, 55)
    c.SetFocalPoint(*fp); c.SetPosition(*(fp[i] + direction[i] * 400 for i in range(3))); c.SetViewUp(*up)
    r.ResetCamera(); c.Zoom(zoom); w.Render()
    f = vtk.vtkWindowToImageFilter(); f.SetInput(w); f.Update()
    pw = vtk.vtkPNGWriter(); pw.SetFileName(os.path.join(out, fn)); pw.SetInputConnection(f.GetOutputPort()); pw.Write()


shot((1.0, -1.4, 0.7), "kf16-angle-valve-iso.png")
shot((-1.2, 1.0, 0.5), "kf16-angle-valve-iso-back.png")
shot((0, -1, 0), "kf16-angle-valve-front.png", zoom=1.1)
