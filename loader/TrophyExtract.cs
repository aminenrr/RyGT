// SPDX-License-Identifier: GPL-2.0-only
// PRT-TROPHYEXTRACT (fork only, pr-tests): RyGTTrophyExtract.exe, a small tool that turns a game's
// sce_sys\trophy\trophy00.trp into the extracted files the emulator and the launcher read:
// user\trophy\<npCommId>\Xml\TROPCONF.XML, Xml\TROP.XML (and TROP_xx.XML) and Icons\*.PNG.
// The emulator itself stays key-free (as upstream asked on shadGT#75 / RyGT#69); the player's own
// trophy key is only used here. Built by loader\build.cmd beside RyGTLauncher.exe, packaged in tools\.
//
// GUI: run it with no arguments.
// Command line: RyGTTrophyExtract.exe <game folder or trophy00.trp> <key hex or key file> [npCommId] [RyGT folder]

using System;
using System.Collections.Generic;
using System.Drawing;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Security.Cryptography;
using System.Text;
using System.Text.RegularExpressions;
using System.Windows.Forms;

namespace RyGTTrophyExtract
{
    class TrpEntry
    {
        public string Name;
        public long Offset, Length;
        public uint Flag;
    }

    static class Trp
    {
        public const uint Magic = 0xDCA24D00;
        const int HeaderSize = 0x60, EntrySize = 0x40;
        static readonly Regex HexKey = new Regex("^[0-9A-Fa-f]{32}$");
        public static readonly Regex NpCommIdPattern = new Regex("NPWR[0-9]{5}_[0-9]{2}");

        public static string CleanKey(string text)
        {
            return new string((text ?? "").Where(c => !char.IsWhiteSpace(c)).ToArray());
        }

        public static bool ValidKey(string hex) { return hex != null && HexKey.IsMatch(hex); }

        public static byte[] HexToBytes(string hex)
        {
            var bytes = new byte[hex.Length / 2];
            for (int i = 0; i < bytes.Length; ++i)
                bytes[i] = byte.Parse(hex.Substring(i * 2, 2), NumberStyles.HexNumber, CultureInfo.InvariantCulture);
            return bytes;
        }

        static uint U32(byte[] b, long at) { return (uint)(b[at] << 24 | b[at + 1] << 16 | b[at + 2] << 8 | b[at + 3]); }
        static long U64(byte[] b, long at) { return (long)((ulong)U32(b, at) << 32 | U32(b, at + 4)); }

        public static List<TrpEntry> Parse(byte[] trp)
        {
            if (trp == null || trp.Length < HeaderSize) throw new InvalidDataException("The trophy file is too short.");
            if (U32(trp, 0) != Magic) throw new InvalidDataException("This is not a TRP trophy file (bad magic).");
            uint count = U32(trp, 0x10), size = U32(trp, 0x14);
            if (size < EntrySize || count > 4096 || HeaderSize + (long)count * size > trp.Length)
                throw new InvalidDataException("The trophy file's entry table is damaged.");
            var list = new List<TrpEntry>();
            for (long i = 0; i < count; ++i)
            {
                long at = HeaderSize + i * size;
                int end = 0;
                while (end < 32 && trp[at + end] != 0) ++end;
                var entry = new TrpEntry
                {
                    Name = Encoding.ASCII.GetString(trp, (int)at, end),
                    Offset = U64(trp, at + 0x20), Length = U64(trp, at + 0x28), Flag = U32(trp, at + 0x30)
                };
                if (entry.Offset < 0 || entry.Length < 0 || entry.Offset + entry.Length > trp.Length)
                    throw new InvalidDataException("The trophy file's entry " + entry.Name + " points outside the file.");
                list.Add(entry);
            }
            return list;
        }

        public static bool SafeName(string name)
        {
            return !string.IsNullOrEmpty(name) && name.IndexOfAny(new[] { '/', '\\', ':' }) < 0 && name.IndexOf("..", StringComparison.Ordinal) < 0 &&
                   name.IndexOfAny(Path.GetInvalidFileNameChars()) < 0;
        }

        public static byte[] Bytes(byte[] trp, TrpEntry entry)
        {
            var data = new byte[entry.Length];
            Buffer.BlockCopy(trp, (int)entry.Offset, data, 0, data.Length);
            return data;
        }

        static RijndaelManaged Aes128(byte[] key, byte[] iv)
        {
            return new RijndaelManaged { BlockSize = 128, KeySize = 128, Mode = CipherMode.CBC, Padding = PaddingMode.None, Key = key, IV = iv };
        }

        static byte[] DeriveKey(byte[] key, string npCommId)
        {
            var block = new byte[16];
            var id = Encoding.ASCII.GetBytes(npCommId ?? "");
            Buffer.BlockCopy(id, 0, block, 0, Math.Min(16, id.Length));
            using (var aes = Aes128(key, new byte[16]))
            using (var enc = aes.CreateEncryptor())
                return enc.TransformFinalBlock(block, 0, 16);
        }

        // The decrypted XML, cut after its last '>' (the rest is padding).
        public static byte[] DecryptEsfm(byte[] key, string npCommId, byte[] entry)
        {
            if (entry == null || entry.Length < 32) throw new InvalidDataException("A trophy XML entry is too short.");
            var iv = new byte[16];
            Buffer.BlockCopy(entry, 0, iv, 0, 16);
            int length = (entry.Length - 16) / 16 * 16;
            byte[] plain;
            using (var aes = Aes128(DeriveKey(key, npCommId), iv))
            using (var dec = aes.CreateDecryptor())
                plain = dec.TransformFinalBlock(entry, 16, length);
            int last = Array.LastIndexOf(plain, (byte)'>');
            var result = new byte[last + 1];
            Buffer.BlockCopy(plain, 0, result, 0, result.Length);
            return result;
        }

        public static bool LooksLikeTrophyConf(byte[] xml)
        {
            if (xml == null || xml.Length == 0) return false;
            string head = Encoding.UTF8.GetString(xml, 0, Math.Min(xml.Length, 4096));
            return head.IndexOf("<trophyconf", StringComparison.Ordinal) >= 0;
        }
    }

    static class Extractor
    {
        static readonly byte[] Entropy = Encoding.ASCII.GetBytes("RyGT trophy extract PRT-TROPHYEXTRACT");

        // The RyGT folder: the tool sits in <RyGT>\tools\.
        public static string DefaultRoot()
        {
            string dir = Path.GetDirectoryName(Application.ExecutablePath);
            if (string.Equals(Path.GetFileName(dir), "tools", StringComparison.OrdinalIgnoreCase))
                dir = Path.GetDirectoryName(dir);
            return dir;
        }

        static string KeyPath() { return Path.Combine(DefaultRoot(), "user", "trophy-extract.key.dpapi"); }

        public static string LoadKey()
        {
            try
            {
                if (!File.Exists(KeyPath())) return "";
                var plain = ProtectedData.Unprotect(File.ReadAllBytes(KeyPath()), Entropy, DataProtectionScope.CurrentUser);
                string hex = Encoding.ASCII.GetString(plain);
                return Trp.ValidKey(hex) ? hex : "";
            }
            catch { return ""; }
        }

        public static void SaveKey(string hex)
        {
            Directory.CreateDirectory(Path.GetDirectoryName(KeyPath()));
            File.WriteAllBytes(KeyPath(), ProtectedData.Protect(Encoding.ASCII.GetBytes(hex), Entropy, DataProtectionScope.CurrentUser));
        }

        public static void ForgetKey()
        {
            try { if (File.Exists(KeyPath())) File.Delete(KeyPath()); } catch { }
        }

        // A key given as 32 hex characters or as a file holding them (or 16 raw bytes).
        public static string ReadKeyArgument(string text)
        {
            string clean = Trp.CleanKey(text);
            if (Trp.ValidKey(clean)) return clean;
            if (File.Exists(text))
            {
                var bytes = File.ReadAllBytes(text);
                if (bytes.Length == 16) return BitConverter.ToString(bytes).Replace("-", "");
                string fromFile = Trp.CleanKey(Encoding.ASCII.GetString(bytes));
                var m = Regex.Match(fromFile, "[0-9A-Fa-f]{32}");
                if (m.Success) return m.Value;
            }
            throw new ArgumentException("The trophy key is 32 hexadecimal characters (or a file holding them).");
        }

        // trophy00.trp from a game folder (CUSAxxxxx or its app0) or the file itself.
        public static string FindTrp(string input)
        {
            if (File.Exists(input)) return input;
            if (Directory.Exists(input))
            {
                foreach (var candidate in new[] {
                    Path.Combine(input, "sce_sys", "trophy", "trophy00.trp"),
                    Path.Combine(input, "app0", "sce_sys", "trophy", "trophy00.trp"),
                    Path.Combine(input, "trophy", "trophy00.trp"),
                    Path.Combine(input, "trophy00.trp") })
                    if (File.Exists(candidate)) return candidate;
            }
            throw new FileNotFoundException("sce_sys\\trophy\\trophy00.trp was not found in " + input);
        }

        // The npCommId from sce_sys\npbind.dat next to the trophy folder, if it is there.
        public static List<string> NpCommIds(string trpPath)
        {
            var ids = new List<string>();
            try
            {
                string trophyDir = Path.GetDirectoryName(trpPath);
                string sceSys = Path.GetDirectoryName(trophyDir);
                foreach (var npbind in new[] { Path.Combine(sceSys, "npbind.dat"), Path.Combine(trophyDir, "npbind.dat") })
                    if (File.Exists(npbind))
                        foreach (Match m in Trp.NpCommIdPattern.Matches(Encoding.ASCII.GetString(File.ReadAllBytes(npbind))))
                            if (!ids.Contains(m.Value)) ids.Add(m.Value);
            }
            catch { }
            return ids;
        }

        // Extracts into <root>\user\trophy\<npCommId>; nothing is written when the key is wrong.
        public static string Run(string trpPath, string keyHex, string npCommId, string root, Action<string> log)
        {
            if (!Trp.ValidKey(keyHex)) throw new ArgumentException("The trophy key is 32 hexadecimal characters.");
            if (string.IsNullOrEmpty(npCommId) || !Trp.NpCommIdPattern.IsMatch(npCommId) || npCommId.Length != 12)
                throw new ArgumentException("The npCommId looks like NPWR12345_00 (from sce_sys\\npbind.dat).");
            byte[] key = Trp.HexToBytes(keyHex);
            byte[] trp = File.ReadAllBytes(trpPath);
            var entries = Trp.Parse(trp);

            var xmls = new Dictionary<string, byte[]>(StringComparer.OrdinalIgnoreCase);
            var icons = new Dictionary<string, byte[]>(StringComparer.OrdinalIgnoreCase);
            foreach (var entry in entries)
            {
                if (!Trp.SafeName(entry.Name)) { log("Skipped an entry with an unsafe name."); continue; }
                if (entry.Flag == 0 && entry.Name.EndsWith(".PNG", StringComparison.OrdinalIgnoreCase))
                    icons[entry.Name] = Trp.Bytes(trp, entry);
                else if (entry.Flag == 3)
                    xmls[entry.Name.Replace("ESFM", "XML")] = Trp.DecryptEsfm(key, npCommId, Trp.Bytes(trp, entry));
                else if (entry.Name.EndsWith(".SFM", StringComparison.OrdinalIgnoreCase))
                    xmls[entry.Name.Substring(0, entry.Name.Length - 4) + ".XML"] = Trp.Bytes(trp, entry);
            }
            byte[] conf;
            if (!xmls.TryGetValue("TROPCONF.XML", out conf))
                throw new InvalidDataException("trophy00.trp has no TROPCONF entry.");
            if (!Trp.LooksLikeTrophyConf(conf))
                throw new InvalidDataException("TROPCONF did not decrypt to <trophyconf>: the key or the npCommId (" + npCommId + ") is wrong.");
            if (!xmls.ContainsKey("TROP.XML")) log("Warning: trophy00.trp has no TROP entry; the names will come from TROPCONF.");

            string trophyRoot = Path.Combine(root, "user", "trophy");
            string target = Path.Combine(trophyRoot, npCommId);
            string temp = target + ".extracting";
            if (Directory.Exists(temp)) Directory.Delete(temp, true);
            Directory.CreateDirectory(Path.Combine(temp, "Xml"));
            Directory.CreateDirectory(Path.Combine(temp, "Icons"));
            foreach (var pair in xmls) File.WriteAllBytes(Path.Combine(temp, "Xml", pair.Key), pair.Value);
            foreach (var pair in icons) File.WriteAllBytes(Path.Combine(temp, "Icons", pair.Key), pair.Value);
            // Keep anything else already in the folder (the launcher's icon cache); replace Xml and Icons.
            Directory.CreateDirectory(target);
            foreach (var sub in new[] { "Xml", "Icons" })
            {
                string old = Path.Combine(target, sub);
                if (Directory.Exists(old)) Directory.Delete(old, true);
                Directory.Move(Path.Combine(temp, sub), old);
            }
            Directory.Delete(temp, true);
            log("Extracted " + xmls.Count + " XML files and " + icons.Count + " icons for " + npCommId + ".");
            return target;
        }
    }

    class MainForm : Form
    {
        readonly TextBox game = new TextBox(), key = new TextBox(), npComm = new TextBox(), root = new TextBox(), output = new TextBox();
        readonly CheckBox remember = new CheckBox();

        public MainForm()
        {
            Text = "RyGT Trophy Extract";
            ClientSize = new Size(620, 330);
            FormBorderStyle = FormBorderStyle.FixedDialog;
            MaximizeBox = false;
            StartPosition = FormStartPosition.CenterScreen;
            Font = new Font("Segoe UI", 9f);

            int y = 12;
            AddRow("Game folder or trophy00.trp", game, ref y, BrowseGame);
            AddRow("Trophy key (32 hex)", key, ref y, null);
            key.UseSystemPasswordChar = true;
            key.Text = Extractor.LoadKey();
            remember.Text = "Remember the key on this PC (encrypted for your Windows user)";
            remember.Checked = key.Text.Length > 0;
            remember.SetBounds(180, y - 4, 420, 22);
            Controls.Add(remember);
            y += 22;
            AddRow("npCommId (auto from npbind.dat)", npComm, ref y, null);
            AddRow("RyGT folder", root, ref y, BrowseRoot);
            root.Text = Extractor.DefaultRoot();

            var extract = new Button { Text = "Extract", Width = 100, Height = 28 };
            extract.SetBounds(180, y, 100, 28);
            extract.Click += delegate { DoExtract(); };
            Controls.Add(extract);
            AcceptButton = extract;
            y += 36;
            output.Multiline = true;
            output.ReadOnly = true;
            output.ScrollBars = ScrollBars.Vertical;
            output.SetBounds(12, y, 596, ClientSize.Height - y - 12);
            Controls.Add(output);
            game.TextChanged += delegate { FillNpCommId(); };
            AllowDrop = true;
            DragEnter += (s, e) => { if (e.Data.GetDataPresent(DataFormats.FileDrop)) e.Effect = DragDropEffects.Copy; };
            DragDrop += (s, e) => { var files = (string[])e.Data.GetData(DataFormats.FileDrop); if (files != null && files.Length > 0) game.Text = files[0]; };
        }

        void AddRow(string label, TextBox box, ref int y, Action browse)
        {
            var text = new Label { Text = label, AutoSize = false };
            text.SetBounds(12, y + 3, 168, 20);
            Controls.Add(text);
            box.SetBounds(180, y, browse != null ? 340 : 428, 23);
            Controls.Add(box);
            if (browse != null)
            {
                var button = new Button { Text = "Browse..." };
                button.SetBounds(526, y - 1, 82, 25);
                button.Click += delegate { browse(); };
                Controls.Add(button);
            }
            y += 32;
        }

        void BrowseGame()
        {
            using (var dialog = new FolderBrowserDialog { Description = "The game folder (CUSAxxxxx) that holds sce_sys\\trophy\\trophy00.trp" })
                if (dialog.ShowDialog(this) == DialogResult.OK) game.Text = dialog.SelectedPath;
        }

        void BrowseRoot()
        {
            using (var dialog = new FolderBrowserDialog { Description = "The RyGT folder (the one with RyGTLauncher.exe)" })
                if (dialog.ShowDialog(this) == DialogResult.OK) root.Text = dialog.SelectedPath;
        }

        void FillNpCommId()
        {
            try
            {
                var ids = Extractor.NpCommIds(Extractor.FindTrp(game.Text.Trim().Trim('"')));
                if (ids.Count > 0) npComm.Text = ids[0];
                if (ids.Count > 1) Log("npbind.dat lists " + string.Join(", ", ids) + "; using " + ids[0] + ".");
            }
            catch { }
        }

        void Log(string line) { output.AppendText(line + Environment.NewLine); }

        void DoExtract()
        {
            try
            {
                string trp = Extractor.FindTrp(game.Text.Trim().Trim('"'));
                string hex = Trp.CleanKey(key.Text);
                if (string.IsNullOrEmpty(npComm.Text.Trim())) FillNpCommId();
                string target = Extractor.Run(trp, hex, npComm.Text.Trim().ToUpperInvariant(), root.Text.Trim().Trim('"'), Log);
                if (remember.Checked) Extractor.SaveKey(hex); else Extractor.ForgetKey();
                Log("Done: " + target);
            }
            catch (Exception e)
            {
                Log("Failed: " + e.Message);
            }
        }
    }

    static class Program
    {
        [System.Runtime.InteropServices.DllImport("kernel32.dll")]
        static extern bool AttachConsole(int processId);

        [STAThread]
        static int Main(string[] args)
        {
            if (args.Length == 0)
            {
                Application.EnableVisualStyles();
                Application.Run(new MainForm());
                return 0;
            }
            AttachConsole(-1); // a winexe: print into the console it was started from
            Console.WriteLine();
            if (args.Length < 2 || args[0] == "/?" || args[0] == "-h" || args[0] == "--help")
            {
                Console.WriteLine("RyGTTrophyExtract.exe <game folder or trophy00.trp> <key hex or key file> [npCommId] [RyGT folder]");
                return args.Length < 2 ? 2 : 0;
            }
            try
            {
                string trp = Extractor.FindTrp(args[0]);
                string hex = Extractor.ReadKeyArgument(args[1]);
                string id = args.Length > 2 ? args[2].ToUpperInvariant() : Extractor.NpCommIds(trp).FirstOrDefault();
                string root = args.Length > 3 ? args[3] : Extractor.DefaultRoot();
                string target = Extractor.Run(trp, hex, id, root, Console.WriteLine);
                Console.WriteLine("Done: " + target);
                return 0;
            }
            catch (Exception e)
            {
                Console.Error.WriteLine("Failed: " + e.Message);
                return 1;
            }
        }
    }
}
