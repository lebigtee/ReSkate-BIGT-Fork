// Prepares the Boneyard's pictures from the artist's Procreate exports (run from PowerShell:
// Add-Type -TypeDefinition (Get-Content BoneyardAssets.cs -Raw) -ReferencedAssemblies System.Drawing).
//  * Strip: the skeleton without its soft glow (the X-ray cuts it into bones; a glow would show
//    as boxes around each one), its edge kept soft.
//  * Grid: a crop of a picture on dark grey with a labelled grid, to measure joints by eye.
//  * Pack: the three skeleton views cropped and scaled into one sheet (assets/bones.png), and the
//    panel artwork cropped (assets/panel.png); prints where each view landed for the code.
using System;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Drawing.Imaging;
using System.Runtime.InteropServices;

public static class BoneyardAssets {
    static byte[] Pixels(Bitmap bmp, out int stride) {
        var data = bmp.LockBits(new Rectangle(0, 0, bmp.Width, bmp.Height), ImageLockMode.ReadOnly, PixelFormat.Format32bppArgb);
        stride = data.Stride;
        var px = new byte[data.Stride * bmp.Height];
        Marshal.Copy(data.Scan0, px, 0, px.Length);
        bmp.UnlockBits(data);
        return px;
    }
    static Bitmap FromPixels(byte[] px, int w, int h, int stride) {
        var bmp = new Bitmap(w, h, PixelFormat.Format32bppArgb);
        var data = bmp.LockBits(new Rectangle(0, 0, w, h), ImageLockMode.WriteOnly, PixelFormat.Format32bppArgb);
        for (int y = 0; y < h; ++y) Marshal.Copy(px, y * stride, data.Scan0 + y * data.Stride, w * 4);
        bmp.UnlockBits(data);
        return bmp;
    }

    // Alpha below `low` is glow and goes; from `low` to `high` it ramps back in, so the bones'
    // own anti-aliased edge stays soft.
    public static Bitmap Strip(string path, int low, int high) {
        using (var src = new Bitmap(path)) {
            int stride;
            var px = Pixels(src, out stride);
            for (int i = 0; i < px.Length; i += 4) {
                int a = px[i + 3];
                int kept = a <= low ? 0 : a >= high ? 255 : (a - low) * 255 / (high - low);
                px[i + 3] = (byte)kept;
            }
            return FromPixels(px, src.Width, src.Height, stride);
        }
    }

    // The smallest rectangle holding every pixel at least this opaque.
    public static Rectangle Bounds(Bitmap bmp, int alpha) {
        int stride;
        var px = Pixels(bmp, out stride);
        int x0 = bmp.Width, y0 = bmp.Height, x1 = -1, y1 = -1;
        for (int y = 0; y < bmp.Height; ++y)
            for (int x = 0; x < bmp.Width; ++x)
                if (px[y * stride + x * 4 + 3] >= alpha) {
                    if (x < x0) x0 = x; if (x > x1) x1 = x; if (y < y0) y0 = y; if (y > y1) y1 = y;
                }
        return x1 < 0 ? Rectangle.Empty : Rectangle.FromLTRB(x0, y0, x1 + 1, y1 + 1);
    }

    // A crop (x, y, w, h of `bmp`) blown up `zoom` times on dark grey, with a grid every `step`
    // source pixels labelled with source coordinates.
    public static void Grid(Bitmap bmp, string path, int x, int y, int w, int h, float zoom, int step) {
        int W = (int)(w * zoom), H = (int)(h * zoom);
        using (var outBmp = new Bitmap(W, H, PixelFormat.Format32bppArgb))
        using (var g = Graphics.FromImage(outBmp))
        using (var minor = new Pen(Color.FromArgb(90, 0, 200, 255), 1))
        using (var major = new Pen(Color.FromArgb(200, 255, 80, 80), 1))
        using (var font = new Font("Segoe UI", 11, FontStyle.Bold, GraphicsUnit.Pixel))
        using (var ink = new SolidBrush(Color.FromArgb(255, 255, 230, 80))) {
            g.Clear(Color.FromArgb(255, 40, 40, 48));
            g.InterpolationMode = InterpolationMode.NearestNeighbor;
            g.DrawImage(bmp, new Rectangle(0, 0, W, H), new Rectangle(x, y, w, h), GraphicsUnit.Pixel);
            for (int gx = (x + step - 1) / step * step; gx <= x + w; gx += step) {
                float sx = (gx - x) * zoom;
                g.DrawLine(gx % (step * 2) == 0 ? major : minor, sx, 0, sx, H);
                if (gx % (step * 2) == 0) g.DrawString(gx.ToString(), font, ink, sx + 2, 2);
            }
            for (int gy = (y + step - 1) / step * step; gy <= y + h; gy += step) {
                float sy = (gy - y) * zoom;
                g.DrawLine(gy % (step * 2) == 0 ? major : minor, 0, sy, W, sy);
                if (gy % (step * 2) == 0) g.DrawString(gy.ToString(), font, ink, 2, sy + 2);
            }
            outBmp.Save(path, ImageFormat.Png);
        }
    }

    public static void Save(Bitmap bmp, string path) { bmp.Save(path, ImageFormat.Png); }

    // The cut-out quads of one view (lines of `py skeleton_spec.py quads`: view, name, four
    // corners, a, b) over that view's drawing on dark grey, to check them by eye.
    public static void Overlay(Bitmap bmp, string[] lines, string view, string path) {
        using (var outBmp = new Bitmap(bmp.Width, bmp.Height, PixelFormat.Format32bppArgb))
        using (var g = Graphics.FromImage(outBmp))
        using (var font = new Font("Segoe UI", 13, FontStyle.Bold, GraphicsUnit.Pixel)) {
            g.Clear(Color.FromArgb(255, 40, 40, 48));
            g.DrawImage(bmp, 0, 0, bmp.Width, bmp.Height);
            g.SmoothingMode = SmoothingMode.AntiAlias;
            Color[] colours = { Color.Orange, Color.DeepSkyBlue, Color.LimeGreen, Color.Magenta, Color.Yellow, Color.Cyan };
            int k = 0;
            foreach (var line in lines) {
                var f = line.Trim().Split(' ');
                if (f.Length < 14 || f[0] != view) continue;
                var c = colours[k++ % colours.Length];
                var pts = new PointF[4];
                for (int i = 0; i < 4; ++i) pts[i] = new PointF(float.Parse(f[2 + i * 2], System.Globalization.CultureInfo.InvariantCulture),
                                                                 float.Parse(f[3 + i * 2], System.Globalization.CultureInfo.InvariantCulture));
                float ax = float.Parse(f[10]), ay = float.Parse(f[11]), bx = float.Parse(f[12]), by = float.Parse(f[13]);
                using (var pen = new Pen(Color.FromArgb(230, c), 2)) g.DrawPolygon(pen, pts);
                using (var pen = new Pen(Color.FromArgb(255, 255, 40, 40), 2)) g.DrawLine(pen, ax, ay, bx, by);
                using (var dot = new SolidBrush(Color.Red)) { g.FillEllipse(dot, ax - 5, ay - 5, 10, 10); g.FillEllipse(dot, bx - 5, by - 5, 10, 10); }
                using (var ink = new SolidBrush(c)) g.DrawString(f[1], font, ink, pts[0].X + 2, pts[0].Y + 2);
            }
            outBmp.Save(path, ImageFormat.Png);
        }
    }

    // Draws `src`'s `crop` scaled by `scale` at (x, y) of `sheet`.
    public static void Place(Bitmap sheet, Bitmap src, Rectangle crop, float scale, int x, int y) {
        using (var g = Graphics.FromImage(sheet)) {
            g.CompositingMode = CompositingMode.SourceCopy;
            g.InterpolationMode = InterpolationMode.HighQualityBicubic;
            g.PixelOffsetMode = PixelOffsetMode.HighQuality;
            using (var wrap = new ImageAttributes()) {
                wrap.SetWrapMode(WrapMode.TileFlipXY); // no dark fringe at the crop's edge
                g.DrawImage(src, new Rectangle(x, y, (int)Math.Round(crop.Width * scale), (int)Math.Round(crop.Height * scale)),
                            crop.X, crop.Y, crop.Width, crop.Height, GraphicsUnit.Pixel, wrap);
            }
        }
    }
    // A copy with these polygons (lines of four corners, x y each) made fully transparent.
    public static Bitmap Clear(Bitmap src, string[] lines) {
        var copy = new Bitmap(src);
        using (var g = Graphics.FromImage(copy))
        using (var clear = new SolidBrush(Color.Transparent)) {
            g.CompositingMode = CompositingMode.SourceCopy;
            g.SmoothingMode = SmoothingMode.AntiAlias;
            foreach (var line in lines) {
                var f = line.Trim().Split(' ');
                if (f.Length < 8) continue;
                var pts = new PointF[4];
                for (int i = 0; i < 4; ++i) pts[i] = new PointF(float.Parse(f[i * 2], System.Globalization.CultureInfo.InvariantCulture),
                                                                 float.Parse(f[i * 2 + 1], System.Globalization.CultureInfo.InvariantCulture));
                g.FillPolygon(clear, pts);
            }
        }
        return copy;
    }
    // A copy keeping only what lies inside these polygons.
    public static Bitmap Keep(Bitmap src, string[] lines) {
        var copy = Blank(src.Width, src.Height);
        using (var g = Graphics.FromImage(copy))
        using (var path = new GraphicsPath()) {
            foreach (var line in lines) {
                var f = line.Trim().Split(' ');
                if (f.Length < 8) continue;
                var pts = new PointF[4];
                for (int i = 0; i < 4; ++i) pts[i] = new PointF(float.Parse(f[i * 2], System.Globalization.CultureInfo.InvariantCulture),
                                                                 float.Parse(f[i * 2 + 1], System.Globalization.CultureInfo.InvariantCulture));
                path.AddPolygon(pts);
            }
            path.FillMode = FillMode.Winding;
            g.SetClip(path);
            g.CompositingMode = CompositingMode.SourceCopy;
            g.DrawImage(src, 0, 0, src.Width, src.Height);
        }
        return copy;
    }
    public static Bitmap Blank(int w, int h) {
        var bmp = new Bitmap(w, h, PixelFormat.Format32bppArgb);
        using (var g = Graphics.FromImage(bmp)) g.Clear(Color.Transparent);
        return bmp;
    }
}
