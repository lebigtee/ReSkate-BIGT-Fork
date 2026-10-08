// Draws the Hall of Meat's X-ray offline, as hall_of_meat_hud.cpp does in game: real skeleton
// poses from `meat probe` recordings (probe_preview.py writes them out), seen by a camera at a few
// angles round the skater, each laid over with the sprite tables (skeleton_spec.py cpp) from
// assets/bones.png as there: the front drawing, with the back's skull when seen from behind.
// Keep place(), twin_of() and seen_from_behind() in step with hall_of_meat_hud.cpp.
using System;
using System.Collections.Generic;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Drawing.Imaging;
using System.Globalization;
using System.Text.RegularExpressions;

public static class HomPreview {
    // Points as hall_of_meat.h numbers them.
    static readonly string[] Points = { "hips", "spine", "spine1", "spine2", "spine3", "neck", "neck1", "head", "skull",
        "right_clavicle", "right_shoulder", "right_elbow", "right_wrist", "right_knuckles",
        "left_clavicle", "left_shoulder", "left_elbow", "left_wrist", "left_knuckles",
        "right_hip", "right_knee", "right_ankle", "right_toes", "left_hip", "left_knee", "left_ankle", "left_toes" };
    static int P(string name) { return Array.IndexOf(Points, name); }

    class Sprite { public int From, To, Bone, Picture; public float Ax, Ay, Bx, By, Before, After, Left, Right; public bool Torso, Twin; }
    static readonly float[,] Pictures = { { 105, 5, 0, 0 }, { 130, 20, 492, 0 }, { 405, 78, 954, 0 }, { 405, 78, 1094, 0 } };
    const float PictureScale = 0.6f;

    // The front table, and the back's skull (its own line, back_skull).
    static List<Sprite> front = new List<Sprite>();
    static Sprite backSkull;
    public static void LoadTables(string[] lines) {
        var row = new Regex(@"\{meat::(\w+), meat::(\w+), (\d+), (\d+), \{(-?\d+), (-?\d+)\}, \{(-?\d+), (-?\d+)\}, (\d+), (\d+), (\d+), (\d+), Breadth::(\w+), (\w+)\}");
        foreach (var line in lines) {
            var m = row.Match(line);
            if (!m.Success) continue;
            Func<int, float> f = i => float.Parse(m.Groups[i].Value, CultureInfo.InvariantCulture);
            var s = new Sprite { From = P(m.Groups[1].Value), To = P(m.Groups[2].Value), Bone = int.Parse(m.Groups[3].Value),
                Picture = int.Parse(m.Groups[4].Value), Ax = f(5), Ay = f(6), Bx = f(7), By = f(8), Before = f(9), After = f(10),
                Left = f(11), Right = f(12), Torso = m.Groups[13].Value == "torso", Twin = m.Groups[14].Value == "true" };
            if (line.Contains("back_skull")) backSkull = s;
            else front.Add(s);
        }
    }
    static int OtherSide(int p) {
        var n = Points[p];
        if (n.StartsWith("right_")) return P("left_" + n.Substring(6));
        if (n.StartsWith("left_")) return P("right_" + n.Substring(5));
        return p;
    }
    static Sprite TwinOf(Sprite s) {
        var t = (Sprite)s.GetType().GetMethod("MemberwiseClone", System.Reflection.BindingFlags.Instance | System.Reflection.BindingFlags.NonPublic).Invoke(s, null);
        t.From = OtherSide(s.From); t.To = OtherSide(s.To);
        t.Bone = s.Bone == 2 || s.Bone == 3 ? 5 - s.Bone : s.Bone >= 7 ? (s.Bone % 2 == 1 ? s.Bone + 1 : s.Bone - 1) : s.Bone;
        t.Ax = 1024 - s.Ax; t.Bx = 1024 - s.Bx; t.Left = s.Right; t.Right = s.Left;
        return t;
    }
    // The front drawing in drawing order (the skull fifth), the back's skull in its place from behind.
    static List<Sprite> Sprites(bool behind) {
        var result = new List<Sprite>();
        for (int i = 0; i < 5; ++i) result.Add(i == 4 && behind && backSkull != null ? backSkull : front[i]);
        for (int i = 5; i < front.Count; ++i) { result.Add(front[i]); if (front[i].Twin) result.Add(TwinOf(front[i])); }
        return result;
    }

    // The camera: rows right, up, back and its position, as GameView has them.
    public class Camera { public float[] Right, Up, Back, Origin; public float Focal; public PointF Centre; }
    static float Dot(float[] a, float[] b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
    static float[] Minus(float[] a, float[] b) { return new[] { a[0] - b[0], a[1] - b[1], a[2] - b[2] }; }
    static float[] Cross(float[] a, float[] b) { return new[] { a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0] }; }
    static float Len(float[] a) { return (float)Math.Sqrt(Dot(a, a)); }
    static bool Project(Camera c, float[] point, out PointF at, out float depth) {
        var d = Minus(point, c.Origin);
        depth = -Dot(d, c.Back);
        at = PointF.Empty;
        if (depth < 0.1f) return false;
        at = new PointF(c.Centre.X + Dot(d, c.Right) * c.Focal / depth, c.Centre.Y - Dot(d, c.Up) * c.Focal / depth);
        return true;
    }

    // The game's state for seen_from_behind: kept between frames, as there.
    public static bool Outlines = false;
    public static bool behind = false;
    public static float chestSign = 0;
    public static bool SeenFromBehind(float[][] pts, Camera cam, float breadth) {
        PointF hips, top, r, l; float depth;
        if (!Project(cam, pts[P("hips")], out hips, out depth) || !Project(cam, pts[P("spine3")], out top, out depth) ||
            !Project(cam, pts[P("right_shoulder")], out r, out depth) || !Project(cam, pts[P("left_shoulder")], out l, out depth)) return behind;
        float ux = top.X - hips.X, uy = top.Y - hips.Y, ax = r.X - l.X, ay = r.Y - l.Y;
        float turn = ux * ay - uy * ax;
        var chest = Cross(Minus(pts[P("spine3")], pts[P("hips")]), Minus(pts[P("right_shoulder")], pts[P("left_shoulder")]));
        var camera = Minus(cam.Origin, pts[P("hips")]);
        if (breadth > 0.6f) chestSign = (turn < 0) == (Dot(chest, camera) > 0) ? 1 : -1;
        float size = Len(chest) * Len(camera);
        if (chestSign == 0 || size < 1e-6f) { behind = turn >= 0; return behind; }
        float facing = chestSign * Dot(chest, camera) / size;
        if (behind ? facing > 0.2f : facing < -0.2f) behind = !behind;
        return behind;
    }

    static PointF OnSheet(int picture, float x, float y) {
        return new PointF(Pictures[picture, 2] + (x - Pictures[picture, 0]) * PictureScale, Pictures[picture, 3] + (y - Pictures[picture, 1]) * PictureScale);
    }

    // One cell: the pose seen by `cam`, laid over with the sprites. Returns the choice made.
    public static string Draw(Graphics g, Bitmap sheet, float[][] pts, Camera cam) {
        float breadth = 1;
        {
            PointF r, l; float dr, dl;
            float real = Len(Minus(pts[P("right_shoulder")], pts[P("left_shoulder")]));
            if (real > 0.01f && Project(cam, pts[P("right_shoulder")], out r, out dr) && Project(cam, pts[P("left_shoulder")], out l, out dl))
                breadth = Math.Max(0.35f, Math.Min(1f, (float)Math.Sqrt((r.X - l.X) * (r.X - l.X) + (r.Y - l.Y) * (r.Y - l.Y)) / (cam.Focal / ((dr + dl) * 0.5f) * real)));
        }
        bool seenBehind = SeenFromBehind(pts, cam, breadth);
        // The skeleton's own lines, faint, to see the sprites sit on it.
        using (var pen = new Pen(Color.FromArgb(120, 255, 80, 80), 2)) {
            int[][] links = { new[] { 0, 1 }, new[] { 1, 2 }, new[] { 2, 3 }, new[] { 3, 4 }, new[] { 4, 5 }, new[] { 5, 6 }, new[] { 6, 7 },
                new[] { 4, 10 }, new[] { 10, 11 }, new[] { 11, 12 }, new[] { 12, 13 }, new[] { 4, 15 }, new[] { 15, 16 }, new[] { 16, 17 }, new[] { 17, 18 },
                new[] { 0, 19 }, new[] { 19, 20 }, new[] { 20, 21 }, new[] { 21, 22 }, new[] { 0, 23 }, new[] { 23, 24 }, new[] { 24, 25 }, new[] { 25, 26 } };
            foreach (var link in links) {
                PointF a, b; float da, db;
                if (Project(cam, pts[link[0]], out a, out da) && Project(cam, pts[link[1]], out b, out db)) g.DrawLine(pen, a, b);
            }
        }
        foreach (var s in Sprites(seenBehind)) {
            PointF a, b; float da, db;
            if (!Project(cam, pts[s.From], out a, out da) || !Project(cam, pts[s.To], out b, out db)) continue;
            float axx = s.Bx - s.Ax, axy = s.By - s.Ay, sheetLength = (float)Math.Sqrt(axx * axx + axy * axy);
            float dx = axx / sheetLength, dy = axy / sheetLength, nx = -dy, ny = dx;
            float metres = Len(Minus(pts[s.From], pts[s.To]));
            float across = cam.Focal / ((da + db) * 0.5f) * metres / sheetLength;
            if (s.Torso) across *= breadth;
            float lx = b.X - a.X, ly = b.Y - a.Y, screenLength = (float)Math.Sqrt(lx * lx + ly * ly);
            float alx = screenLength > 0.5f ? lx / screenLength : 0, aly = screenLength > 0.5f ? ly / screenLength : 1;
            float stretch = Math.Max(screenLength / sheetLength, across * 0.25f);
            float normx = -aly, normy = alx;
            float[,] corners = { { -s.Before, s.Left }, { sheetLength + s.After, s.Left }, { sheetLength + s.After, -s.Right }, { -s.Before, -s.Right } };
            var sheetPts = new PointF[4];
            var screen = new PointF[4];
            for (int i = 0; i < 4; ++i) {
                float u = corners[i, 0], v = corners[i, 1];
                sheetPts[i] = OnSheet(s.Picture, s.Ax + dx * u + nx * v, s.Ay + dy * u + ny * v);
                screen[i] = new PointF(a.X + alx * u * stretch + normx * v * across, a.Y + aly * u * stretch + normy * v * across);
            }
            // The affine map from the sheet's quad to the screen's.
            var m = Affine(sheetPts[0], sheetPts[1], sheetPts[3], screen[0], screen[1], screen[3]);
            if (m == null) continue;
            var state = g.Save();
            g.MultiplyTransform(m); // on top of the cell's own offset
            using (var clip = new GraphicsPath()) { clip.AddPolygon(sheetPts); g.SetClip(clip, CombineMode.Intersect); }
            g.DrawImage(sheet, 0, 0, sheet.Width, sheet.Height);
            g.Restore(state);
            if (Outlines) using (var pen = new Pen(Color.FromArgb(160, 80, 200, 255), 1)) g.DrawPolygon(pen, screen);
        }
        return (seenBehind ? "front, back skull" : "front") + " breadth " + breadth.ToString("0.00");
    }
    static Matrix Affine(PointF s0, PointF s1, PointF s2, PointF d0, PointF d1, PointF d2) {
        float det = (s1.X - s0.X) * (s2.Y - s0.Y) - (s2.X - s0.X) * (s1.Y - s0.Y);
        if (Math.Abs(det) < 1e-6f) return null;
        // d = M * (s - s0) + d0, with M from the two edge vectors.
        float ex1 = s1.X - s0.X, ey1 = s1.Y - s0.Y, ex2 = s2.X - s0.X, ey2 = s2.Y - s0.Y;
        float fx1 = d1.X - d0.X, fy1 = d1.Y - d0.Y, fx2 = d2.X - d0.X, fy2 = d2.Y - d0.Y;
        float m11 = (fx1 * ey2 - fx2 * ey1) / det, m21 = (fx2 * ex1 - fx1 * ex2) / det;
        float m12 = (fy1 * ey2 - fy2 * ey1) / det, m22 = (fy2 * ex1 - fy1 * ex2) / det;
        float dxo = d0.X - (m11 * s0.X + m21 * s0.Y), dyo = d0.Y - (m12 * s0.X + m22 * s0.Y);
        return new Matrix(m11, m12, m21, m22, dxo, dyo);
    }

    // A camera `distance` metres from the hips, `yaw` degrees round from where the feet point,
    // a little above, looking at them; checked to see the world the right way round.
    public static Camera Around(float[][] pts, float yaw, float distance, int width, int height) {
        var hips = pts[P("hips")];
        var feet = new[] { (pts[P("right_toes")][0] - pts[P("right_ankle")][0] + pts[P("left_toes")][0] - pts[P("left_ankle")][0]) / 2, 0f,
                           (pts[P("right_toes")][2] - pts[P("right_ankle")][2] + pts[P("left_toes")][2] - pts[P("left_ankle")][2]) / 2 };
        float n = (float)Math.Sqrt(feet[0] * feet[0] + feet[2] * feet[2]);
        float fx = feet[0] / n, fz = feet[2] / n, c = (float)Math.Cos(yaw * Math.PI / 180), s = (float)Math.Sin(yaw * Math.PI / 180);
        var dir = new[] { fx * c - fz * s, 0f, fx * s + fz * c };
        var origin = new[] { hips[0] + dir[0] * distance, hips[1] + 0.4f, hips[2] + dir[2] * distance };
        var back = Minus(origin, hips); float bl = Len(back); back = new[] { back[0] / bl, back[1] / bl, back[2] / bl };
        var up = new[] { 0f, 1f, 0f };
        var right = Cross(up, back); float rl = Len(right); right = new[] { right[0] / rl, right[1] / rl, right[2] / rl };
        var camUp = Cross(back, right);
        return new Camera { Right = right, Up = camUp, Back = back, Origin = origin, Focal = height * 1.2f, Centre = new PointF(width / 2f, height / 2f) };
    }

    public static void Sheet(string sheetPath, string[] tableLines, string[] poses, string outPath) {
        LoadTables(tableLines);
        using (var sheet = new Bitmap(sheetPath)) {
            float[] yaws = { 0, 60, 90, 180, 270, 300 };
            int cw = 300, ch = 420;
            using (var outBmp = new Bitmap(cw * yaws.Length, ch * poses.Length, PixelFormat.Format32bppArgb))
            using (var g = Graphics.FromImage(outBmp))
            using (var font = new Font("Segoe UI", 12, FontStyle.Bold, GraphicsUnit.Pixel))
            using (var ink = new SolidBrush(Color.Yellow)) {
                g.Clear(Color.FromArgb(255, 36, 36, 44));
                g.InterpolationMode = InterpolationMode.HighQualityBilinear;
                for (int row = 0; row < poses.Length; ++row) {
                    var parts = poses[row].Trim().Split(' ');
                    var pts = new float[27][];
                    for (int i = 0; i < 27; ++i) pts[i] = new[] { float.Parse(parts[1 + i * 3], CultureInfo.InvariantCulture),
                        float.Parse(parts[2 + i * 3], CultureInfo.InvariantCulture), float.Parse(parts[3 + i * 3], CultureInfo.InvariantCulture) };
                    for (int col = 0; col < yaws.Length; ++col) {
                        var cam = Around(pts, yaws[col], 3.2f, cw, ch);
                        // The game learns the chest's side once front or back is plain: so does this.
                        facing = 0; chestSign = 0;
                        bool unused;
                        FacingOf(pts, Around(pts, 0, 3.2f, cw, ch), 1.0f, out unused);
                        var state = g.Save();
                        g.TranslateTransform(col * cw, row * ch);
                        g.SetClip(new Rectangle(0, 0, cw, ch));
                        var what = Draw(g, sheet, pts, cam);
                        g.DrawString(parts[0] + " | yaw " + yaws[col] + " | " + what, font, ink, 4, 4);
                        g.Restore(state);
                        using (var frame = new Pen(Color.FromArgb(255, 80, 80, 90))) g.DrawRectangle(frame, col * cw, row * ch, cw - 1, ch - 1);
                    }
                }
                outBmp.Save(outPath, ImageFormat.Png);
            }
        }
    }
}
