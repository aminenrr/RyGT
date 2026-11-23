// SPDX-License-Identifier: GPL-2.0-only

using System;
using System.Collections.Generic;
using System.Drawing;
using System.Drawing.Imaging;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Text;
using System.Text.RegularExpressions;
using System.Windows.Forms;
using System.Xml;

namespace RyGT
{
    class TrophyInfo
    {
        public int Id;
        public string IdText = "", Name = "", Detail = "";
        public char Grade = 'B';
        public bool Hidden, Earned;
        public DateTime? Unlocked;
    }

    // FEAT-GT7-187: the trophy files the player already extracted (plain XML). RyGT does not
    // decrypt the game's trophy files.
    static class TrophyXml
    {
        public static readonly Regex NpCommIdPattern = new Regex("NPWR[0-9]{5}_[0-9]{2}");

        public static bool LooksLikeTrophyConf(string xml)
        {
            if (string.IsNullOrEmpty(xml)) return false;
            string head = xml.TrimStart('\uFEFF');
            for (;;)
            {
                head = head.TrimStart(' ', '\r', '\n', '\t');
                string close = head.StartsWith("<?", StringComparison.Ordinal) ? "?>" :
                               head.StartsWith("<!--", StringComparison.Ordinal) ? "-->" : null;
                if (close == null) break;
                int end = head.IndexOf(close, StringComparison.Ordinal);
                if (end < 0) break;
                head = head.Substring(end + close.Length);
            }
            if (head.StartsWith("<trophyconf", StringComparison.Ordinal)) return true;
            return xml.Substring(0, Math.Min(xml.Length, 4096)).IndexOf("<trophyconf", StringComparison.Ordinal) >= 0;
        }

        public static string Describe(string text)
        {
            if (string.IsNullOrEmpty(text)) return "unreadable bytes";
            string head = text.Substring(0, Math.Min(text.Length, 512));
            int printable = head.Count(c => c == '\r' || c == '\n' || c == '\t' || (c >= ' ' && c < 0x7f));
            if (printable < head.Length * 9 / 10) return "unreadable bytes";
            var tag = Regex.Match(head, "<([A-Za-z][A-Za-z0-9_-]*)");
            return tag.Success ? "XML starting with <" + tag.Groups[1].Value + ">" : "text without XML";
        }

        public static List<TrophyInfo> ParseTrophies(string xml)
        {
            var doc = new XmlDocument { XmlResolver = null };
            int start = xml.IndexOf('<');
            doc.LoadXml(start > 0 ? xml.Substring(start) : xml);
            var list = new List<TrophyInfo>();
            foreach (XmlElement node in doc.GetElementsByTagName("trophy"))
            {
                var t = new TrophyInfo { IdText = node.GetAttribute("id") };
                int.TryParse(t.IdText, NumberStyles.Integer, CultureInfo.InvariantCulture, out t.Id);
                t.Hidden = node.GetAttribute("hidden").Equals("yes", StringComparison.OrdinalIgnoreCase);
                string grade = node.GetAttribute("ttype").ToUpperInvariant();
                t.Grade = grade.Length == 1 && "BSGP".IndexOf(grade[0]) >= 0 ? grade[0] : 'B';
                var name = node["name"];
                var detail = node["detail"];
                t.Name = name != null ? name.InnerText.Trim() : "";
                t.Detail = detail != null ? detail.InnerText.Trim() : "";
                list.Add(t);
            }
            return list;
        }

        static readonly Regex TrophyTag = new Regex("<trophy\\b[^>]*>", RegexOptions.IgnoreCase);

        static string Attribute(string tag, string name)
        {
            var m = Regex.Match(tag, "\\b" + name + "\\s*=\\s*\"([^\"]*)\"", RegexOptions.IgnoreCase);
            return m.Success ? m.Groups[1].Value : null;
        }

        public static Dictionary<int, DateTime?> ParseUnlocks(string xml)
        {
            var earned = new Dictionary<int, DateTime?>();
            foreach (Match m in TrophyTag.Matches(xml ?? ""))
            {
                string state = Attribute(m.Value, "unlockstate"), id = Attribute(m.Value, "id");
                int number;
                if (state == null || !state.Equals("true", StringComparison.OrdinalIgnoreCase) || id == null ||
                    !int.TryParse(id, NumberStyles.Integer, CultureInfo.InvariantCulture, out number))
                    continue;
                earned[number] = Timestamp(Attribute(m.Value, "timestamp"));
            }
            return earned;
        }

        public static DateTime? Timestamp(string text)
        {
            long value;
            if (text == null || !long.TryParse(text.Trim(), NumberStyles.Integer, CultureInfo.InvariantCulture, out value) || value <= 0)
                return null;
            try
            {
                var epoch = new DateTime(1970, 1, 1, 0, 0, 0, DateTimeKind.Utc);
                DateTime utc;
                if (value >= 30000000000000000L) utc = new DateTime(value * 10, DateTimeKind.Utc);
                else if (value >= 100000000000000L) utc = epoch.AddTicks(value * 10);
                else if (value >= 100000000000L) utc = epoch.AddMilliseconds(value);
                else utc = epoch.AddSeconds(value);
                return utc.ToLocalTime();
            }
            catch { return null; }
        }
    }

    static class TrophyStore
    {
        public static string UserDir(string root) { return Path.Combine(root, "user"); }
        // Left by launchers that kept a trophy key (removed): deleted when the Trophies page opens.
        static string OldKeyPath(string root) { return Path.Combine(UserDir(root), "trophy.key.dpapi"); }
        static string PendingPath(string root) { return Path.Combine(UserDir(root), "trophy.keyfile.pending"); }
        public static string TrophyDir(string root) { return Path.Combine(UserDir(root), "trophy"); }
        static string IniPath(string root) { return Path.Combine(UserDir(root), "trophy.ini"); }
        public static string AssetsDir(string root) { return Path.Combine(UserDir(root), "trophy_assets"); }

        public static bool ToastsOn(string root)
        {
            try
            {
                if (!File.Exists(IniPath(root))) return true;
                foreach (var line in File.ReadAllLines(IniPath(root)))
                    if (line.Trim().Replace(" ", "") == "toast=0") return false;
            }
            catch { }
            return true;
        }

        public static void SetToasts(string root, bool on)
        {
            Directory.CreateDirectory(UserDir(root));
            var lines = File.Exists(IniPath(root)) ? File.ReadAllLines(IniPath(root)).ToList() : new List<string>();
            lines.RemoveAll(l => l.TrimStart().StartsWith("toast=", StringComparison.OrdinalIgnoreCase));
            lines.Add("toast=" + (on ? "1" : "0"));
            File.WriteAllLines(IniPath(root), lines);
        }

        public static char TestToastPending(string root)
        {
            try
            {
                if (File.Exists(IniPath(root)))
                    foreach (var line in File.ReadAllLines(IniPath(root)))
                    {
                        string l = line.Trim().Replace(" ", "");
                        if (l.StartsWith("testtoast=", StringComparison.OrdinalIgnoreCase) && l.Length > 10 && "BSGP".IndexOf(char.ToUpperInvariant(l[10])) >= 0)
                            return char.ToUpperInvariant(l[10]);
                    }
            }
            catch { }
            return '\0';
        }

        public static void SetTestToast(string root, char grade)
        {
            Directory.CreateDirectory(UserDir(root));
            var lines = File.Exists(IniPath(root)) ? File.ReadAllLines(IniPath(root)).ToList() : new List<string>();
            lines.RemoveAll(l => l.TrimStart().StartsWith("testtoast=", StringComparison.OrdinalIgnoreCase));
            if (grade != '\0') lines.Add("testtoast=" + grade);
            File.WriteAllLines(IniPath(root), lines);
        }

        public static char TakeTestToast(string root)
        {
            char grade = TestToastPending(root);
            if (grade != '\0') { try { SetTestToast(root, '\0'); } catch { } }
            return grade;
        }

        public static void CleanUpOldKeyFiles(string root)
        {
            try { if (File.Exists(OldKeyPath(root))) File.Delete(OldKeyPath(root)); } catch { }
            try
            {
                if (!File.Exists(PendingPath(root))) return;
                string path = File.ReadAllText(PendingPath(root)).Trim();
                string temp = Path.GetFullPath(Path.GetTempPath());
                if (path.Length > 0 && Path.GetFullPath(path).StartsWith(temp, StringComparison.OrdinalIgnoreCase) &&
                    File.Exists(path) && new FileInfo(path).Length <= 64)
                {
                    try { File.WriteAllBytes(path, new byte[32]); } catch { }
                    File.Delete(path);
                }
                File.Delete(PendingPath(root));
            }
            catch { }
        }

        public static void ApplyEnvironment(string root, System.Diagnostics.ProcessStartInfo start)
        {
            if (!ToastsOn(root)) start.EnvironmentVariables["RYGT_TROPHY_TOAST"] = "0";
            try { WriteBuiltinAssets(root); } catch { }
            char test = TakeTestToast(root);
            if (test != '\0') start.EnvironmentVariables["RYGT_TROPHY_TOAST_TEST"] = test.ToString();
        }

        public static List<string> App0Layers(string root, string update, string game)
        {
            var folders = new List<string> { App0Layout.Top(root, GameInfo.GT7.TitleId) };
            string env = Environment.GetEnvironmentVariable("RYGT_APP0_LAYERS");
            if (!string.IsNullOrEmpty(env))
            {
                var layers = env.Split(';').Select(s => s.Trim()).Where(s => s.Length > 0).ToList();
                layers.Reverse();
                folders.AddRange(layers);
                return folders;
            }
            string ini = GameInfo.GT7.IniPath(root);
            if (!File.Exists(ini)) ini = Path.Combine(root, "RyGT.ini");
            try
            {
                if (File.Exists(ini))
                    foreach (var line in File.ReadAllLines(ini))
                    {
                        int eq = line.IndexOf('=');
                        if (eq < 0) continue;
                        string key = line.Substring(0, eq).Trim(), value = line.Substring(eq + 1).Trim();
                        if (key == "update" && value.Length > 0) update = value;
                        else if (key == "game" && value.Length > 0) game = value;
                    }
            }
            catch { }
            if (!string.IsNullOrEmpty(update)) folders.Add(update);
            if (!string.IsNullOrEmpty(game)) folders.Add(game);
            return folders;
        }

        public static string FindGameFile(List<string> layers, string relative)
        {
            foreach (var folder in layers)
            {
                try
                {
                    string path = Path.Combine(folder, relative);
                    if (File.Exists(path)) return path;
                }
                catch { }
            }
            return null;
        }

        public static List<string> NpCommIds(string root, List<string> layers)
        {
            var ids = new List<string>();
            string npbind = FindGameFile(layers, Path.Combine("sce_sys", "npbind.dat"));
            try
            {
                if (npbind != null)
                    foreach (Match m in TrophyXml.NpCommIdPattern.Matches(Encoding.ASCII.GetString(File.ReadAllBytes(npbind))))
                        if (!ids.Contains(m.Value)) ids.Add(m.Value);
            }
            catch { }
            try
            {
                string trophy = Path.Combine(UserDir(root), "trophy");
                if (Directory.Exists(trophy))
                    foreach (var dir in Directory.GetDirectories(trophy))
                    {
                        var m = TrophyXml.NpCommIdPattern.Match(Path.GetFileName(dir));
                        if (m.Success && !ids.Contains(m.Value)) ids.Add(m.Value);
                    }
                foreach (var file in UnlockFiles(root, null))
                {
                    var m = TrophyXml.NpCommIdPattern.Match(Path.GetFileName(file));
                    if (m.Success && !ids.Contains(m.Value)) ids.Add(m.Value);
                }
            }
            catch { }
            return ids;
        }

        public static string ProgressDir(string root) { return Path.Combine(root, "user", "savedata", GameInfo.GT7.TitleId, "_sd", "trophies_progress"); }

        public static string OldProgressDir(string root) { return Path.Combine(root, "_sd", "trophies_progress"); }

        public static List<string> UnlockFiles(string root, string npCommId)
        {
            var dirs = new List<string>();
            foreach (string progress in new[] { ProgressDir(root), OldProgressDir(root) })
            {
                dirs.Add(progress);
                try { if (Directory.Exists(progress)) dirs.AddRange(Directory.GetDirectories(progress).OrderBy(d => d)); }
                catch { }
            }
            string home = Path.Combine(UserDir(root), "home");
            string first = Path.Combine(home, "1000");
            dirs.Add(Path.Combine(first, "trophy"));
            try
            {
                if (Directory.Exists(home))
                    dirs.AddRange(Directory.GetDirectories(home).Where(d => !d.Equals(first, StringComparison.OrdinalIgnoreCase))
                                           .OrderBy(d => d).Select(d => Path.Combine(d, "trophy")));
            }
            catch { }
            var list = new List<string>();
            foreach (var dir in dirs)
            {
                try
                {
                    if (!Directory.Exists(dir)) continue;
                    if (npCommId != null)
                    {
                        string file = Path.Combine(dir, npCommId + ".xml");
                        if (File.Exists(file)) list.Add(file);
                    }
                    else list.AddRange(Directory.GetFiles(dir, "NPWR*.xml"));
                }
                catch { }
            }
            return list;
        }

        public static readonly string[] BuiltinFiles = { "bronze.png", "silver.png", "gold.png", "platinum.png", "trophy.wav", "platinum.wav", "font.ttf", "font-LICENSE.txt" };
        public static string BuiltinDir(string root) { return Path.Combine(UserDir(root), "trophy_builtin"); }
        public const string ReadmeName = "README.txt";

        static Stream BuiltinStream(string name)
        {
            return typeof(TrophyStore).Assembly.GetManifestResourceStream("trophy." + name);
        }

        public static int WriteBuiltinAssets(string root)
        {
            string dir = BuiltinDir(root);
            int present = 0;
            foreach (var name in BuiltinFiles)
            {
                try
                {
                    using (var stream = BuiltinStream(name))
                    {
                        if (stream == null) continue;
                        Directory.CreateDirectory(dir);
                        string path = Path.Combine(dir, name);
                        if (!File.Exists(path) || new FileInfo(path).Length != stream.Length)
                        {
                            string temp = path + ".new";
                            using (var file = File.Create(temp)) stream.CopyTo(file);
                            if (File.Exists(path)) File.Delete(path);
                            File.Move(temp, path);
                        }
                        present++;
                    }
                }
                catch { }
            }
            return present;
        }

        public static string AssetFile(string root, params string[] names)
        {
            foreach (var dir in new[] { AssetsDir(root), BuiltinDir(root) })
                foreach (var name in names)
                {
                    string path = Path.Combine(dir, name);
                    if (File.Exists(path)) return path;
                }
            return null;
        }
        public static string CupFile(string root, char grade)
        {
            string name = grade == 'P' ? "platinum" : grade == 'G' ? "gold" : grade == 'S' ? "silver" : "bronze";
            return AssetFile(root, name + ".png");
        }
        public static string SoundFile(string root, bool platinum)
        {
            return platinum ? AssetFile(root, "platinum.wav", "ps5_platinum.wav") : AssetFile(root, "trophy.wav", "ps5_trophy.wav");
        }
        public static string FontFile(string root) { return AssetFile(root, "font.ttf", "font.otf", Path.Combine("fonts", "SSTRg.ttf")); }

        public const string ReadmeText =
"ryGT7 custom trophy pop-up files\r\n" +
"================================\r\n" +
"\r\n" +
"Put your own pictures, sounds or font in this folder to change the in-game trophy pop-up.\r\n" +
"Any file you leave out uses ryGT7's built-in one. Changes apply the next time you play.\r\n" +
"\r\n" +
"Cup pictures (the small cup next to the trophy name)\r\n" +
"  bronze.png  silver.png  gold.png  platinum.png\r\n" +
"  PNG with a transparent background, square (same width and height).\r\n" +
"  128 x 128 or larger; 256 x 256 is recommended. They are drawn at about 30 px at 1080p.\r\n" +
"\r\n" +
"Sounds\r\n" +
"  trophy.wav    plays for Bronze, Silver and Gold\r\n" +
"  platinum.wav  plays for the Platinum trophy\r\n" +
"  WAV, uncompressed PCM, 16-bit, 44.1 kHz or 48 kHz, mono or stereo. Keep them short (under 5 s).\r\n" +
"  MP3 or OGG files are not played.\r\n" +
"\r\n" +
"Font (the trophy name and the \"You earned a trophy!\" line)\r\n" +
"  font.ttf  or  font.otf\r\n" +
"  TrueType or OpenType. Names in Chinese, Japanese or Korean always use a built-in font\r\n" +
"  that carries those characters.\r\n" +
"\r\n" +
"Names are not case sensitive. Older names still work: ps5_trophy.wav, ps5_platinum.wav and\r\n" +
"fonts\\SSTRg.ttf.\r\n" +
"\r\n" +
"The trophy's own picture (the large square on the left) comes from the game and cannot be replaced.\r\n" +
"Only use files you have the right to use.\r\n";

        public static string EnsureCustomFolder(string root)
        {
            string dir = AssetsDir(root);
            Directory.CreateDirectory(dir);
            string readme = Path.Combine(dir, ReadmeName);
            try
            {
                if (!File.Exists(readme) || File.ReadAllText(readme) != ReadmeText)
                    File.WriteAllText(readme, ReadmeText, Encoding.UTF8);
            }
            catch { }
            return dir;
        }
    }

    class TrophySet
    {
        public string NpCommId, Folder, TitleName = "";
        public List<TrophyInfo> Trophies = new List<TrophyInfo>();
        public Dictionary<string, Image> Icons = new Dictionary<string, Image>(StringComparer.OrdinalIgnoreCase);
        public string UnlockFile;

        // The set whose extracted files are in user\trophy\<npCommId> (Xml\TROPCONF.XML, Xml\TROP.XML, Icons).
        public static string FindExtracted(string root, List<string> layers, out string folder)
        {
            var ids = TrophyStore.NpCommIds(root, layers);
            foreach (var id in ids)
            {
                string dir = Path.Combine(TrophyStore.TrophyDir(root), id);
                string conf = Path.Combine(dir, "Xml", "TROPCONF.XML");
                try
                {
                    if (File.Exists(conf) && TrophyXml.LooksLikeTrophyConf(File.ReadAllText(conf)))
                    {
                        folder = dir;
                        return id;
                    }
                }
                catch { }
            }
            folder = null;
            string where = ids.Count > 0 ? Path.Combine(TrophyStore.TrophyDir(root), ids[0]) : Path.Combine(TrophyStore.TrophyDir(root), "<npCommId>");
            throw new FileNotFoundException("The trophy files are not there. Put the extracted trophy files (Xml\\TROPCONF.XML, Xml\\TROP.XML and the Icons folder) in " +
                                            where + ". Trophies stay off in the game until they are.");
        }

        public static TrophySet Load(string root, List<string> layers)
        {
            string folder;
            string id = FindExtracted(root, layers, out folder);
            var set = new TrophySet { NpCommId = id, Folder = folder };
            string tropPath = Path.Combine(folder, "Xml", "TROP.XML");
            if (!File.Exists(tropPath)) tropPath = Path.Combine(folder, "Xml", "TROPCONF.XML");
            string xml = File.ReadAllText(tropPath);
            set.Trophies = TrophyXml.ParseTrophies(xml);
            var title = Regex.Match(xml, "<title-name>([^<]*)</title-name>");
            if (title.Success) set.TitleName = System.Net.WebUtility.HtmlDecode(title.Groups[1].Value.Trim());
            string icons = Path.Combine(folder, "Icons");
            string cache = Path.Combine(folder, "launcher_icons");
            if (Directory.Exists(icons))
                foreach (var path in Directory.GetFiles(icons, "*.PNG"))
                {
                    string name = Path.GetFileName(path).ToUpperInvariant();
                    string cached = Path.Combine(cache, name);
                    try
                    {
                        if (File.Exists(cached))
                        {
                            set.Icons[name] = LoadBitmap(File.ReadAllBytes(cached));
                            continue;
                        }
                    }
                    catch { }
                    try
                    {
                        using (var stream = new MemoryStream(File.ReadAllBytes(path)))
                        using (var image = Image.FromStream(stream))
                        {
                            var small = new Bitmap(56, 56, PixelFormat.Format32bppArgb);
                            using (var g = Graphics.FromImage(small))
                            {
                                g.InterpolationMode = System.Drawing.Drawing2D.InterpolationMode.HighQualityBicubic;
                                g.DrawImage(image, new Rectangle(0, 0, 56, 56));
                            }
                            set.Icons[name] = small;
                            try
                            {
                                Directory.CreateDirectory(cache);
                                small.Save(cached, ImageFormat.Png);
                            }
                            catch { }
                        }
                    }
                    catch { }
                }
            var unlocks = TrophyStore.UnlockFiles(root, id);
            if (unlocks.Count > 0)
            {
                set.UnlockFile = unlocks[0];
                try
                {
                    var earned = TrophyXml.ParseUnlocks(File.ReadAllText(unlocks[0]));
                    foreach (var t in set.Trophies)
                    {
                        DateTime? when;
                        if (earned.TryGetValue(t.Id, out when)) { t.Earned = true; t.Unlocked = when; }
                    }
                }
                catch { }
            }
            return set;
        }

        static Bitmap LoadBitmap(byte[] bytes)
        {
            using (var stream = new MemoryStream(bytes))
            using (var image = Image.FromStream(stream))
                return new Bitmap(image);
        }

        public Dictionary<int, Image> RowIcons = new Dictionary<int, Image>();

        public void PrepareRowIcons()
        {
            foreach (var t in Trophies)
            {
                if (t.Hidden && !t.Earned) continue;
                Image icon = IconFor(t);
                if (icon == null) continue;
                RowIcons[t.Id] = t.Earned ? icon : Greyed(icon);
            }
        }

        static Image Greyed(Image image)
        {
            var grey = new Bitmap(image.Width, image.Height);
            using (var g = Graphics.FromImage(grey))
            using (var attributes = new ImageAttributes())
            {
                attributes.SetColorMatrix(new ColorMatrix(new[]
                {
                    new[] { .3f, .3f, .3f, 0, 0 }, new[] { .59f, .59f, .59f, 0, 0 }, new[] { .11f, .11f, .11f, 0, 0 },
                    new[] { 0f, 0, 0, .55f, 0 }, new[] { 0f, 0, 0, 0, 1 }
                }));
                g.DrawImage(image, new Rectangle(0, 0, image.Width, image.Height), 0, 0, image.Width, image.Height, GraphicsUnit.Pixel, attributes);
            }
            return grey;
        }

        public Image IconFor(TrophyInfo t)
        {
            Image image;
            return Icons.TryGetValue("TROP" + t.Id.ToString("000", CultureInfo.InvariantCulture) + ".PNG", out image) ? image : null;
        }
    }

    partial class LoaderForm
    {
        TableLayoutPanel trophyList;
        Label trophySummary;
        bool trophiesLoaded;

        List<string> TrophyLayers() { return TrophyStore.App0Layers(root, settings.Update, settings.Game); }

        void BuildTrophiesPage()
        {
            TrophyStore.CleanUpOldKeyFiles(root);
            trophiesLoaded = false;
            var page = NewPage("Trophies", "Trophies",
                "Gran Turismo 7's trophies, read from trophy files you already extracted from your own game.");

            var filesSection = Section(page, "Trophy files");
            filesSection.Controls.Add(Hint("Put the extracted trophy files in user\\trophy\\<npCommId> (for GT7, NPWR followed by 5 digits, _ and 2 digits): " +
                                           "Xml\\TROPCONF.XML, Xml\\TROP.XML and the Icons folder. Without them the game runs without trophies."));
            filesSection.Controls.Add(OpenRow("The trophy folder", OpenTrophyFolder, "Open trophy folder"));

            var popups = Section(page, "Pop-ups");
            var toast = new ToggleRow { Text = "Show trophy pop-ups", Checked = TrophyStore.ToastsOn(root) };
            toast.CheckedChanged += delegate
            {
                try { TrophyStore.SetToasts(root, toast.Checked); }
                catch (Exception e) { MessageBox.Show(this, "Could not save the setting: " + e.Message, "Trophies"); }
            };
            popups.Controls.Add(toast);
            popups.Controls.Add(OpenRow("What a trophy pop-up looks like and sounds like", delegate { ShowTrophyPreview(null); }, "Preview"));
            popups.Controls.Add(OpenRow("Your own cup pictures, sounds and font", OpenCustomTrophyFolder, "Open custom trophy images and sounds folder"));
            popups.Controls.Add(Hint("The launcher has its own cups, sounds and font. Files you put in this folder replace them; its README.txt lists the names, " +
                                     "sizes and formats. Changes apply the next time you play."));

            var listSection = Section(page, "Trophies");
            var head = new TableLayoutPanel { ColumnCount = 2, AutoSize = true, Dock = DockStyle.Fill, Margin = new Padding(0, 6, 0, 6), BackColor = Color.Transparent };
            head.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
            head.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));
            trophySummary = new Label { Text = "", AutoSize = true, Anchor = AnchorStyles.Left, Font = new Font("Segoe UI Semibold", 11f), Margin = new Padding(0, 6, 0, 0) };
            var refresh = new Button { Text = "Refresh", AutoSize = true, Margin = new Padding(0) };
            refresh.Click += delegate { trophiesLoaded = false; RefreshTrophies(); };
            head.Controls.Add(trophySummary, 0, 0);
            head.Controls.Add(refresh, 1, 0);
            listSection.Controls.Add(head);
            trophyList = new TableLayoutPanel { ColumnCount = 1, AutoSize = true, Dock = DockStyle.Fill, Margin = new Padding(0), BackColor = Color.Transparent };
            trophyList.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
            listSection.Controls.Add(trophyList);

            pages["Trophies"].VisibleChanged += delegate { if (pages["Trophies"].Visible && !trophiesLoaded) RefreshTrophies(); };
        }

        void ShowTrophyPreview(string header)
        {
            try { TrophyStore.WriteBuiltinAssets(root); } catch { }
            using (var form = new TrophyPreviewForm(root, lastTrophySet, header))
                form.ShowDialog(this);
        }

        void OpenTrophyFolder()
        {
            try
            {
                string dir = TrophyStore.TrophyDir(root);
                var ids = TrophyStore.NpCommIds(root, TrophyLayers());
                if (ids.Count > 0) dir = Path.Combine(dir, ids[0]);
                Directory.CreateDirectory(dir);
                System.Diagnostics.Process.Start("explorer.exe", "\"" + dir + "\"");
            }
            catch (Exception e) { MessageBox.Show(this, "Could not open the folder: " + e.Message, "Trophies"); }
        }

        void OpenCustomTrophyFolder()
        {
            try
            {
                string dir = TrophyStore.EnsureCustomFolder(root);
                System.Diagnostics.Process.Start("explorer.exe", "\"" + dir + "\"");
            }
            catch (Exception e) { MessageBox.Show(this, "Could not open the folder: " + e.Message, "Trophies"); }
        }

        TrophySet lastTrophySet;
        int trophyGeneration;

        void RefreshTrophies()
        {
            if (trophyList == null) return;
            trophiesLoaded = true;
            int generation = ++trophyGeneration;
            ClearTrophyList();
            trophySummary.Text = "Loading trophies\u2026";
            string where = root;
            var layers = TrophyLayers();
            System.Threading.ThreadPool.QueueUserWorkItem(delegate
            {
                TrophySet set = null;
                Exception error = null;
                try
                {
                    set = TrophySet.Load(where, layers);
                    set.PrepareRowIcons();
                }
                catch (Exception e) { error = e; }
                try
                {
                    if (!IsDisposed && IsHandleCreated)
                        BeginInvoke((Action)delegate { ShowTrophies(generation, set, error); });
                }
                catch { }
            });
        }

        void ClearTrophyList()
        {
            trophyList.SuspendLayout();
            foreach (Control old in trophyList.Controls.Cast<Control>().ToList())
            {
                trophyList.Controls.Remove(old);
                old.Dispose();
            }
            trophyList.ResumeLayout();
        }

        void ShowTrophies(int generation, TrophySet set, Exception error)
        {
            if (generation != trophyGeneration || trophyList == null || trophyList.IsDisposed) return;
            ClearTrophyList();
            trophyList.SuspendLayout();
            if (error != null)
            {
                trophySummary.Text = "No trophy files";
                trophyList.Controls.Add(Hint(error.Message));
                trophyList.ResumeLayout();
                return;
            }
            lastTrophySet = set;
            int earned = set.Trophies.Count(t => t.Earned), total = set.Trophies.Count;
            trophySummary.Text = earned + " of " + total + " earned (" + (total == 0 ? 0 : (int)Math.Round(100.0 * earned / total)) + "%)";
            if (set.UnlockFile == null)
                trophyList.Controls.Add(Hint("Nothing earned yet (no progress file for " + set.NpCommId + " in user\\savedata\\" + GameInfo.GT7.TitleId + "\\_sd\\trophies_progress)."));
            foreach (var t in set.Trophies.OrderBy(t => t.Id))
            {
                Image icon;
                set.RowIcons.TryGetValue(t.Id, out icon);
                trophyList.Controls.Add(new TrophyRowView(t, icon));
            }
            trophyList.ResumeLayout();
        }

        internal static readonly Dictionary<char, Color> GradeColors = new Dictionary<char, Color>
        {
            { 'B', Color.FromArgb(205, 127, 50) }, { 'S', Color.FromArgb(192, 196, 204) },
            { 'G', Color.FromArgb(230, 190, 60) }, { 'P', Color.FromArgb(150, 200, 235) }
        };
        internal static readonly Dictionary<char, string> GradeNames = new Dictionary<char, string>
        {
            { 'B', "Bronze" }, { 'S', "Silver" }, { 'G', "Gold" }, { 'P', "Platinum" }
        };
    }

    class TrophyRowView : Control
    {
        static readonly Font NameFont = new Font("Segoe UI Semibold", 10f);
        static readonly Font DetailFont = new Font("Segoe UI", 9f);
        readonly TrophyInfo trophy;
        readonly Image icon;

        public TrophyRowView(TrophyInfo trophy, Image icon)
        {
            this.trophy = trophy;
            this.icon = icon;
            SetStyle(ControlStyles.AllPaintingInWmPaint | ControlStyles.OptimizedDoubleBuffer | ControlStyles.UserPaint |
                     ControlStyles.ResizeRedraw | ControlStyles.SupportsTransparentBackColor, true);
            BackColor = Color.Transparent;
            Height = 76;
            Dock = DockStyle.Fill;
            Margin = new Padding(0);
            Tag = "heading";
        }

        protected override void OnPaint(PaintEventArgs e)
        {
            var g = e.Graphics;
            bool secret = trophy.Hidden && !trophy.Earned;
            var picture = new Rectangle(0, (Height - 56) / 2, 56, 56);
            if (icon != null) g.DrawImage(icon, picture);
            else using (var fill = new SolidBrush(Dark.Card)) g.FillRectangle(fill, picture);

            string when = trophy.Earned ? (trophy.Unlocked.HasValue ? "Earned " + trophy.Unlocked.Value.ToString("g", CultureInfo.CurrentCulture) : "Earned") : "Not earned";
            string grade = LoaderForm.GradeNames[trophy.Grade];
            Color stateColor = trophy.Earned ? LoaderForm.GradeColors[trophy.Grade] : Dark.DimText;
            var flags = TextFormatFlags.Right | TextFormatFlags.NoPrefix | TextFormatFlags.SingleLine;
            int stateWidth = Math.Max(TextRenderer.MeasureText(g, grade, DetailFont).Width, TextRenderer.MeasureText(g, when, DetailFont).Width);
            int right = Width - 2;
            TextRenderer.DrawText(g, grade, DetailFont, new Rectangle(right - stateWidth, 18, stateWidth, 18), stateColor, flags);
            TextRenderer.DrawText(g, when, DetailFont, new Rectangle(right - stateWidth, 38, stateWidth, 18), stateColor, flags);

            int x = 72, width = Math.Max(40, right - stateWidth - 16 - x);
            TextRenderer.DrawText(g, secret ? "Hidden trophy" : trophy.Name, NameFont, new Rectangle(x, 10, width, 22),
                                  trophy.Earned ? Dark.Text : Dark.DimText, TextFormatFlags.NoPrefix | TextFormatFlags.SingleLine | TextFormatFlags.EndEllipsis);
            TextRenderer.DrawText(g, secret ? "Keep playing to reveal this trophy." : trophy.Detail, DetailFont, new Rectangle(x, 32, width, 38),
                                  Dark.DimText, TextFormatFlags.NoPrefix | TextFormatFlags.WordBreak | TextFormatFlags.EndEllipsis);
            using (var line = new Pen(Dark.Line)) g.DrawLine(line, 0, Height - 1, Width, Height - 1);
        }
    }

    class TrophyPreviewForm : Form
    {
        readonly string root;
        readonly TrophySet set;
        readonly System.Drawing.Text.PrivateFontCollection fonts = new System.Drawing.Text.PrivateFontCollection();
        readonly Dictionary<char, Image> cups = new Dictionary<char, Image>();
        readonly Dictionary<char, Button> gradeButtons = new Dictionary<char, Button>();
        readonly CardView card;
        readonly CheckBox nextPlay;
        Font titleFont, subtitleFont;
        System.Media.SoundPlayer player;
        char grade = 'G';
        bool loading = true;

        public TrophyPreviewForm(string root, TrophySet set, string header)
        {
            this.root = root;
            this.set = set;
            Text = "Trophy pop-up preview";
            Font = new Font("Segoe UI", 9f);
            StartPosition = FormStartPosition.CenterParent;
            FormBorderStyle = FormBorderStyle.FixedDialog;
            MaximizeBox = false;
            MinimizeBox = false;
            AutoSize = true;
            AutoSizeMode = AutoSizeMode.GrowAndShrink;
            try { Icon = Icon.ExtractAssociatedIcon(Application.ExecutablePath); } catch { }
            LoadAssets();

            var outer = new TableLayoutPanel { AutoSize = true, ColumnCount = 1, Padding = new Padding(18, 14, 18, 14) };
            Controls.Add(outer);
            if (header != null)
                outer.Controls.Add(new Label { Text = header, AutoSize = true, MaximumSize = new Size(700, 0), Font = new Font("Segoe UI Semibold", 10f), Margin = new Padding(0, 0, 0, 10), UseMnemonic = false });
            var grades = new FlowLayoutPanel { AutoSize = true, WrapContents = false, Margin = new Padding(0, 0, 0, 10) };
            foreach (char g in "BSGP")
            {
                char pick = g;
                var button = new Button { Text = LoaderForm.GradeNames[g], AutoSize = true, Margin = new Padding(0, 0, 8, 0) };
                button.Click += delegate { SelectGrade(pick, true); };
                gradeButtons[g] = button;
                grades.Controls.Add(button);
            }
            var replay = new Button { Text = "Play sound", AutoSize = true, Margin = new Padding(16, 0, 0, 0) };
            replay.Click += delegate { PlaySound(); };
            grades.Controls.Add(replay);
            outer.Controls.Add(grades);

            card = new CardView(this) { Size = new Size(700, 152), Margin = new Padding(0) };
            outer.Controls.Add(card);

            char pending = TrophyStore.TestToastPending(root);
            nextPlay = new CheckBox { Text = "Show this test pop-up the next time I play (once)", AutoSize = true, Checked = pending != '\0', Margin = new Padding(0, 10, 0, 0) };
            nextPlay.CheckedChanged += delegate { SaveNextPlay(); };
            outer.Controls.Add(nextPlay);
            outer.Controls.Add(new Label
            {
                Text = "Uses ryGT7's built-in cups, sounds and font unless your custom folder has its own.", AutoSize = true,
                MaximumSize = new Size(700, 0), ForeColor = Dark.DimText, Tag = "heading", Margin = new Padding(0, 6, 0, 10)
            });
            var close = new Button { Text = "Close", AutoSize = true, DialogResult = DialogResult.OK, Anchor = AnchorStyles.Right, Margin = new Padding(0) };
            outer.Controls.Add(close);
            AcceptButton = close;
            CancelButton = close;

            SelectGrade(pending != '\0' ? pending : 'G', false);
            loading = false;
            Dark.Apply(this);
            HandleCreated += delegate { Dark.TitleBar(this); };
            Shown += delegate { PlaySound(); };
        }

        void LoadAssets()
        {
            foreach (char g in "BSGP")
            {
                string path = TrophyStore.CupFile(root, g);
                if (path == null) continue;
                try
                {
                    using (var stream = new MemoryStream(File.ReadAllBytes(path)))
                    using (var image = Image.FromStream(stream))
                        cups[g] = new Bitmap(image);
                }
                catch { }
            }
            string font = TrophyStore.FontFile(root);
            if (font != null)
            {
                try
                {
                    fonts.AddFontFile(font);
                    var family = fonts.Families.Length > 0 ? fonts.Families[0] : null;
                    if (family != null)
                    {
                        var style = family.IsStyleAvailable(FontStyle.Regular) ? FontStyle.Regular : FontStyle.Bold;
                        titleFont = new Font(family, 30f, style, GraphicsUnit.Pixel);
                        subtitleFont = new Font(family, 24f, style, GraphicsUnit.Pixel);
                    }
                }
                catch { titleFont = null; subtitleFont = null; }
            }
            if (titleFont == null) titleFont = new Font("Segoe UI Semibold", 30f, FontStyle.Regular, GraphicsUnit.Pixel);
            if (subtitleFont == null) subtitleFont = new Font("Segoe UI Semibold", 24f, FontStyle.Regular, GraphicsUnit.Pixel);
        }

        void SelectGrade(char g, bool sound)
        {
            grade = g;
            foreach (var pair in gradeButtons)
            {
                pair.Value.Tag = pair.Key == g ? "accent" : null;
                pair.Value.Invalidate();
            }
            card.Invalidate();
            if (!loading) SaveNextPlay();
            if (sound) PlaySound();
        }

        void SaveNextPlay()
        {
            if (loading) return;
            try { TrophyStore.SetTestToast(root, nextPlay.Checked ? grade : '\0'); }
            catch (Exception e) { MessageBox.Show(this, "Could not save the setting: " + e.Message, "Trophies"); }
        }

        void PlaySound()
        {
            string path = TrophyStore.SoundFile(root, grade == 'P');
            if (path == null) return;
            try
            {
                if (player != null) { player.Stop(); player.Dispose(); }
                player = new System.Media.SoundPlayer(path);
                player.Play();
            }
            catch { }
        }

        TrophyInfo Sample()
        {
            if (set == null) return null;
            return set.Trophies.Where(t => t.Grade == grade).OrderBy(t => t.Hidden ? 1 : 0).ThenBy(t => t.Id).FirstOrDefault();
        }

        protected override void Dispose(bool disposing)
        {
            if (disposing)
            {
                if (player != null) { try { player.Stop(); } catch { } player.Dispose(); }
                foreach (var image in cups.Values) image.Dispose();
                if (titleFont != null) titleFont.Dispose();
                if (subtitleFont != null) subtitleFont.Dispose();
                fonts.Dispose();
            }
            base.Dispose(disposing);
        }

        class CardView : Control
        {
            readonly TrophyPreviewForm form;
            public CardView(TrophyPreviewForm form)
            {
                this.form = form;
                SetStyle(ControlStyles.AllPaintingInWmPaint | ControlStyles.OptimizedDoubleBuffer | ControlStyles.UserPaint | ControlStyles.ResizeRedraw, true);
                BackColor = Color.FromArgb(70, 76, 88);
                Tag = "heading";
            }

            static System.Drawing.Drawing2D.GraphicsPath Rounded(RectangleF r, float radius)
            {
                var path = new System.Drawing.Drawing2D.GraphicsPath();
                float d = radius * 2;
                path.AddArc(r.X, r.Y, d, d, 180, 90);
                path.AddArc(r.Right - d, r.Y, d, d, 270, 90);
                path.AddArc(r.Right - d, r.Bottom - d, d, d, 0, 90);
                path.AddArc(r.X, r.Bottom - d, d, d, 90, 90);
                path.CloseFigure();
                return path;
            }

            void DrawCup(Graphics g, RectangleF r)
            {
                Image cup;
                if (form.cups.TryGetValue(form.grade, out cup)) { g.DrawImage(cup, r); return; }
                using (var brush = new SolidBrush(LoaderForm.GradeColors[form.grade])) g.FillEllipse(brush, r);
            }

            protected override void OnPaint(PaintEventArgs e)
            {
                var g = e.Graphics;
                g.SmoothingMode = System.Drawing.Drawing2D.SmoothingMode.AntiAlias;
                g.InterpolationMode = System.Drawing.Drawing2D.InterpolationMode.HighQualityBicubic;
                g.TextRenderingHint = System.Drawing.Text.TextRenderingHint.AntiAliasGridFit;
                using (var backdrop = new System.Drawing.Drawing2D.LinearGradientBrush(ClientRectangle, Color.FromArgb(58, 72, 92), Color.FromArgb(24, 26, 32), 20f))
                    g.FillRectangle(backdrop, ClientRectangle);
                float x = 20, y = 20, cw = 660, ch = 112;
                for (int i = 8; i >= 1; --i)
                    using (var shadow = new SolidBrush(Color.FromArgb(10, 0, 0, 0)))
                    using (var path = Rounded(new RectangleF(x - i, y + 6 - i, cw + 2 * i, ch + 2 * i), 16 + i))
                        g.FillPath(shadow, path);
                using (var fill = new SolidBrush(Color.FromArgb(232, 28, 28, 30)))
                using (var path = Rounded(new RectangleF(x, y, cw, ch), 16))
                    g.FillPath(fill, path);

                bool plat = form.grade == 'P';
                var sample = form.Sample();
                Image art = null;
                if (sample != null && form.set != null) art = form.set.IconFor(sample);
                var artRect = new RectangleF(x + 20, y + (ch - 72) / 2, 72, 72);
                using (var path = Rounded(artRect, 6))
                {
                    if (art != null)
                    {
                        var state = g.Save();
                        g.SetClip(path);
                        g.DrawImage(art, artRect);
                        g.Restore(state);
                    }
                    else
                    {
                        using (var fill = new SolidBrush(Color.FromArgb(44, 46, 54))) g.FillPath(fill, path);
                        DrawCup(g, new RectangleF(artRect.X + 8, artRect.Y + 8, 56, 56));
                    }
                }

                float tx = x + 114;
                if (!plat) DrawCup(g, new RectangleF(tx, y + 22, 30, 30));
                float ttx = plat ? tx : tx + 38, right = x + cw - 20;
                string name = sample != null ? sample.Name : "Test trophy";
                var format = new StringFormat(StringFormatFlags.NoWrap) { Trimming = StringTrimming.EllipsisCharacter };
                using (var title = new SolidBrush(Color.FromArgb(245, 245, 247)))
                    g.DrawString(name, form.titleFont, title, new RectangleF(ttx, y + 17, right - ttx, 40), format);
                using (var subtitle = new SolidBrush(Color.FromArgb(178, 178, 184)))
                    g.DrawString(plat ? "You earned the Platinum trophy!" : "You earned a trophy!", form.subtitleFont, subtitle, new RectangleF(tx, y + 59, right - tx, 34), format);
            }
        }
    }
}
