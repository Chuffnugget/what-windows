using System;
using System.Diagnostics;
using System.Collections.Generic;
using System.Drawing;
using System.IO;
using System.IO.Compression;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Text.RegularExpressions;
using System.Windows.Forms;

internal static class SetupLayoutTests {
    const int Windows11Build = 22621;
    static Size windowLimit;
    [DllImport("user32.dll")]
    static extern bool GetClientRect(IntPtr window, out Rectangle rectangle);

    static void CheckCompatibility(string package, string native, int windowsBuild, bool allowed) {
        string issue = SetupProgram.ArchitectureIssue(package, native, windowsBuild);
        if ((issue.Length == 0) != allowed)
            throw new Exception("Compatibility check failed: package=" + package + ", native=" + native
                + ", Windows build=" + windowsBuild + ", expected=" + (allowed ? "allowed" : "blocked")
                + ", result=" + issue);
    }

    static void CheckPlatformRequirements() {
        // Architecture cases use a supported OS, independently of the CI host.
        CheckCompatibility("arm64", "AMD64", Windows11Build, false);
        CheckCompatibility("arm64", "ARM64", Windows11Build, true);
        CheckCompatibility("x64", "AMD64", Windows11Build, true);
        CheckCompatibility("x64", "ARM64", Windows11Build, true);
        CheckCompatibility("x64", "x86", Windows11Build, false);
        foreach (int build in new[] { 19045, 20348, 21999 }) {
            CheckCompatibility("x64", "AMD64", build, false);
            CheckCompatibility("arm64", "ARM64", build, false);
        }
        CheckCompatibility("x64", "AMD64", 22000, true);
        CheckCompatibility("arm64", "ARM64", 22000, false);
        CheckCompatibility("arm64", "ARM64", 22620, false);
        CheckCompatibility("arm64", "ARM64", 22621, true);
        Console.WriteLine("PASS: architecture and Windows version requirements, including Server 2022");
    }

    static void CheckMetrics(string path) {
        string qml = File.ReadAllText(path);
        foreach (var field in typeof(SetupTheme).GetFields(BindingFlags.Static | BindingFlags.NonPublic)) {
            string role = Char.ToLowerInvariant(field.Name[0]) + field.Name.Substring(1);
            // Colours are checked by ControllerTests::installerDefaultColours,
            // which evaluates the QML bindings for the default blue theme.
            if (field.FieldType == typeof(int)) {
                var match = Regex.Match(qml, @"readonly\s+property\s+int\s+" + role + @"\s*:\s*(\d+)\b");
                if (!match.Success || Int32.Parse(match.Groups[1].Value) != (int)field.GetValue(null))
                    throw new Exception("Installer metric differs from Theme.qml: " + role);
            }
        }
        Console.WriteLine("PASS: installer metrics match QML");
    }

    static T Field<T>(SetupWindow form, string name) {
        return (T)typeof(SetupWindow).GetField(name, BindingFlags.Instance | BindingFlags.NonPublic).GetValue(form);
    }

    static void ScaleFonts(Control control, float scale) {
        // Save children's fonts before changing their inherited parent font.
        foreach (Control child in control.Controls) ScaleFonts(child, scale);
        control.Font = new Font(control.Font.FontFamily, control.Font.Size * scale, control.Font.Style);
    }

    static void Check(Form form, string state, string output) {
        // Observe the normal event-loop layout. Forcing PerformLayout here hid
        // startup clipping by correcting the installer's stale frame geometry.
        Application.DoEvents();
        Rectangle client;
        if (!GetClientRect(form.Handle, out client) || client.Size != form.ClientSize)
            throw new Exception(state + ": cached client size differs from the actual window");
        var frame = form.Controls[0];
        if (frame.Bounds != client)
            throw new Exception(state + ": installer frame does not fit the actual client area");
        var padding = frame.Padding;
        if (padding.Left != padding.Top || padding.Left != padding.Right || padding.Left != padding.Bottom)
            throw new Exception(state + ": outer padding must be equal on all four sides");
        if (!windowLimit.IsEmpty && (form.Width > windowLimit.Width || form.Height > windowLimit.Height))
            throw new Exception(state + ": window exceeds the available display area");
        FlowLayoutPanel actions = null;
        foreach (Control child in form.Controls[0].Controls)
            if (child is FlowLayoutPanel) actions = (FlowLayoutPanel)child;
        if (actions == null) throw new Exception(state + ": missing action buttons");
        var viewport = form.Controls[0].Controls[0] as Panel;
        if (viewport != null && viewport.AutoScroll) {
            if (viewport.Left != padding.Left || viewport.Top != padding.Top
                || client.Width - actions.Right != padding.Right)
                throw new Exception(state + ": content and buttons do not respect the outer padding");
            if (viewport.Bottom > actions.Top || viewport.Right > form.ClientSize.Width)
                throw new Exception(state + ": scrollable content overlaps the action buttons");
            if (viewport.Controls[0].Height > viewport.ClientSize.Height && !viewport.VerticalScroll.Visible)
                throw new Exception(state + ": overflowing content cannot be scrolled");
        }
        int actionHeight = 0;
        foreach (Control button in actions.Controls) {
            if (!button.Visible) continue;
            if (actionHeight != 0 && button.Height != actionHeight)
                throw new Exception(state + ": " + button.Text + " height " + button.Height
                    + " differs from " + actionHeight + "; minimum " + button.MinimumSize.Height
                    + ", font " + button.Font.Size + ", padding " + button.Padding);
            actionHeight = button.Height;
            var bounds = form.RectangleToClient(button.RectangleToScreen(button.ClientRectangle));
            if (!client.Contains(bounds))
                throw new Exception(state + ": clipped button " + button.Text + " at " + bounds);
        }
        int bottom = form.PointToClient(actions.PointToScreen(new Point(0, actions.Height))).Y;
        if (Math.Abs(client.Height - bottom - form.Controls[0].Padding.Bottom) > 2)
            throw new Exception(state + ": inconsistent bottom padding: " + (client.Height - bottom)
                + "; expected " + form.Controls[0].Padding.Bottom);
        using (var image = new Bitmap(form.Width, form.Height)) {
            form.DrawToBitmap(image, new Rectangle(Point.Empty, image.Size));
            image.Save(Path.Combine(output, state + ".png"));
        }
        Console.WriteLine("PASS: " + state);
    }

    static void ShowScaled(Form form, float scale) {
        form.ShowInTaskbar = false;
        form.StartPosition = FormStartPosition.Manual;
        form.Location = new Point(-32000, -32000);
        if (scale != 1f) {
            form.SuspendLayout();
            ScaleFonts(form, scale);
            form.Scale(new SizeF(scale, scale));
            form.ResumeLayout(true);
        }
        if (!windowLimit.IsEmpty) form.MaximumSize = windowLimit;
        form.Show();
    }

    static void CheckPrompts(float scale, string output) {
        string suffix = "-" + (int)(scale * 100);
        const string update = "WHAT 0.0.1.15 is already installed.\r\n\r\n"
            + "Update using version 0.0.1.16? Your settings and sensor selections will be kept. "
            + "No uninstall or profile reset will be performed.\r\n\r\n"
            + "Current installation: C:\\Program Files\\WindowsApps\\WHAT.Desktop_0.0.1.15_x64__z2ppg34bp4p5w";
        using (var form = new Form()) {
            SetupMessage.Configure(form, "Update WHAT?", update, new[] { "Continue", "Cancel" }, new[] { DialogResult.Yes, DialogResult.No });
            if (form.AcceptButton.DialogResult != DialogResult.No || form.CancelButton.DialogResult != DialogResult.No)
                throw new Exception("Update confirmation must keep Cancel as its default keyboard action.");
            ShowScaled(form, scale);
            Check(form, "update" + suffix, output);
        }
        using (var form = new Form()) {
            string message = update.Replace("Update", "Downgrade") + "\r\n\r\nOlder versions may not understand settings saved by newer versions. Back up your profile before continuing.";
            SetupMessage.Configure(form, "Downgrade WHAT?", message, new[] { "Continue", "Cancel" }, new[] { DialogResult.Yes, DialogResult.No });
            ShowScaled(form, scale);
            Check(form, "downgrade" + suffix, output);
        }
        using (var form = new RunningAppPrompt()) {
            ShowScaled(form, scale);
            Check(form, "running-app" + suffix, output);
        }
        using (var form = new Form()) {
            SetupMessage.Configure(form, "Could not open WHAT", "Windows could not start the installed app.", new[] { "OK" }, new[] { DialogResult.OK });
            ShowScaled(form, scale);
            Check(form, "message" + suffix, output);
        }
    }

    static void CheckLogRetention() {
        string root = Path.Combine(Path.GetTempPath(), "WHAT-log-retention-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(root);
        try {
            var folders = new string[7];
            for (int index = 0; index < folders.Length; ++index) {
                folders[index] = Path.Combine(root, Guid.NewGuid().ToString("N"));
                Directory.CreateDirectory(folders[index]);
                File.WriteAllText(Path.Combine(folders[index], "install.log"), "fixture");
                Directory.SetLastWriteTimeUtc(folders[index], DateTime.UtcNow.AddMinutes(index - 10));
            }
            string unrelated = Path.Combine(root, "personal-notes");
            Directory.CreateDirectory(unrelated);
            File.WriteAllText(Path.Combine(unrelated, "keep.txt"), "keep");
            SetupProgram.PruneLogs(root, folders[6]);
            if (Directory.Exists(folders[0]) || Directory.Exists(folders[1])
                || !Directory.Exists(folders[2]) || !Directory.Exists(folders[6])
                || !File.Exists(Path.Combine(unrelated, "keep.txt")))
                throw new Exception("Installer log retention removed the wrong folders.");
            // Unknown files inside a run directory must also survive cleanup.
            File.WriteAllText(Path.Combine(folders[2], "keep.txt"), "keep");
            string current = Path.Combine(root, Guid.NewGuid().ToString("N"));
            Directory.CreateDirectory(current);
            SetupProgram.PruneLogs(root, current);
            if (!File.Exists(Path.Combine(folders[2], "keep.txt")))
                throw new Exception("Installer cleanup removed unrelated content.");
            Console.WriteLine("PASS: installer log retention");
        } finally { Directory.Delete(root, true); }
    }

    static void CheckPackageArchitecture() {
        foreach (string architecture in new[] { "x64", "arm64", "x86", "" }) {
            using (var stream = new MemoryStream()) {
                using (var archive = new ZipArchive(stream, ZipArchiveMode.Create, true))
                using (var writer = new StreamWriter(archive.CreateEntry("AppxManifest.xml").Open())) {
                    string attribute = architecture.Length == 0 ? "" : " ProcessorArchitecture='" + architecture + "'";
                    writer.Write("<Package xmlns='http://schemas.microsoft.com/appx/manifest/foundation/windows10'><Identity"
                        + attribute + " /></Package>");
                }
                stream.Position = 0;
                bool valid = architecture == "x64" || architecture == "arm64";
                try {
                    string actual = SetupProgram.PackageArchitecture(stream);
                    if (!valid || actual != architecture) throw new Exception("Incorrect embedded package architecture.");
                } catch (InvalidDataException) {
                    if (valid) throw;
                }
            }
        }
        Console.WriteLine("PASS: embedded MSIX architecture validation");
    }

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    static extern bool SetProp(IntPtr window, string name, IntPtr value);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    static extern uint RegisterWindowMessage(string name);

    sealed class ShutdownFixtureWindow : NativeWindow, IDisposable {
        readonly uint quitMessage = RegisterWindowMessage("WHAT.QuitForInstaller.1");
        internal ShutdownFixtureWindow(bool supported) {
            // Hidden native window: exercise the real cross-process request
            // without launching WHAT, showing prompts or installing a package.
            CreateHandle(new CreateParams { Caption = "WHAT shutdown fixture" });
            SetProp(Handle, "WHAT.MainWindow", new IntPtr(1));
            if (supported) SetProp(Handle, "WHAT.InstallerQuit", new IntPtr(1));
        }
        protected override void WndProc(ref Message message) {
            if ((uint)message.Msg == quitMessage) {
                Console.WriteLine("dedicated shutdown received");
                Application.ExitThread();
                return;
            }
            if (message.Msg == 0x0010) return; // Normal window close is deliberately vetoed.
            base.WndProc(ref message);
        }
        public void Dispose() { DestroyHandle(); }
    }

    static int RunShutdownFixture(string mode) {
        if (mode == "stalled" || mode == "exited") {
            Console.WriteLine("ready");
            Console.Out.Flush();
            if (mode == "stalled") System.Threading.Thread.Sleep(System.Threading.Timeout.Infinite);
            return 0;
        }
        using (var window = new ShutdownFixtureWindow(mode == "current")) {
            Console.WriteLine("ready");
            Console.Out.Flush();
            Application.Run();
        }
        Console.WriteLine("normal cleanup completed");
        return 0;
    }

    static void CheckShutdown() {
        foreach (string mode in new[] { "current", "legacy", "stalled", "exited" }) {
            var start = new ProcessStartInfo {
                FileName = Assembly.GetExecutingAssembly().Location,
                Arguments = "--shutdown-fixture " + mode,
                UseShellExecute = false, CreateNoWindow = true,
                RedirectStandardOutput = true, RedirectStandardError = true
            };
            using (var process = Process.Start(start)) {
                try {
                    var ready = process.StandardOutput.ReadLineAsync();
                    if (!ready.Wait(5000) || ready.Result != "ready")
                        throw new Exception("Shutdown fixture did not start: " + mode);
                    if (mode == "exited" && !process.WaitForExit(5000))
                        throw new Exception("Already-exited fixture did not exit.");
                    var logs = new List<string>();
                    SetupProgram.CloseRunningProcess(process, logs.Add, mode == "stalled" ? 100 : 3000, 5000);
                    if (!process.HasExited) throw new Exception("Shutdown left the fixture running: " + mode);
                    string output = process.StandardOutput.ReadToEnd();
                    string log = String.Join("\n", logs);
                    if (mode == "current" && !output.Contains("dedicated shutdown received"))
                        throw new Exception("Setup did not use the dedicated WHAT shutdown message.");
                    if ((mode == "current" || mode == "legacy") &&
                        (process.ExitCode != 0 || !output.Contains("normal cleanup completed") || log.Contains("forced termination")))
                        throw new Exception("Graceful shutdown fell back to killing the fixture: " + mode);
                    if (mode == "stalled" && !log.Contains("forced termination completed"))
                        throw new Exception("The stalled-process fallback was not exercised.");
                    if (mode == "exited" && !log.Contains("already exited"))
                        throw new Exception("An exited process was sent another shutdown request.");
                    Console.WriteLine("PASS: installer shutdown " + mode);
                } finally {
                    if (!process.HasExited) { process.Kill(); process.WaitForExit(5000); }
                }
            }
        }
    }

    [STAThread]
    static int Main(string[] args) {
        Application.EnableVisualStyles();
        Application.SetCompatibleTextRenderingDefault(false);
        if (args.Length == 2 && args[0] == "--shutdown-fixture") return RunShutdownFixture(args[1]);
        try {
            CheckShutdown();
            CheckMetrics(args[1]);
            CheckPackageArchitecture();
            CheckLogRetention();
            CheckPlatformRequirements();
            using (var process = Process.Start(SetupProgram.PowerShellStartInfo(
                "Write-Progress -Activity 'Fixture'; Write-Output 'fixture output'; [Console]::Error.WriteLine('fixture failure'); exit 7", args[0]))) {
                var stdout = process.StandardOutput.ReadToEndAsync();
                var stderr = process.StandardError.ReadToEndAsync();
                process.WaitForExit();
                string text = stdout.Result + stderr.Result;
                if (process.ExitCode != 7 || !text.Contains("fixture output") || !text.Contains("fixture failure")
                    || text.Contains("CLIXML") || text.Contains("<Objs"))
                    throw new Exception("Installer PowerShell must preserve failure text and exit code without CLIXML.");
            }
            Console.WriteLine("PASS: architecture checks and plain-text PowerShell failures");
            foreach (Size limit in new[] { Size.Empty, new Size(1024, 768) }) {
                windowLimit = limit;
                string output = Path.Combine(args[0], limit.IsEmpty ? "unrestricted" : "1024x768");
                Directory.CreateDirectory(output);
                CheckLayouts(output);
            }
            return 0;
        } catch (Exception error) {
            Console.Error.WriteLine(error.Message);
            return 1;
        }
    }

    static void CheckLayouts(string output) {
        foreach (float scale in new[] { 1f, 1.25f, 1.5f, 1.75f, 2f }) {
            CheckPrompts(scale, output);
            using (var mismatch = new SetupWindow("arm64", "AMD64", Windows11Build)) {
                ShowScaled(mismatch, scale);
                if (Field<Button>(mismatch, "install").Enabled || Field<Button>(mismatch, "install").Visible
                    || mismatch.AcceptButton != Field<Button>(mismatch, "close")
                    || !Field<Label>(mismatch, "status").Text.Contains("Windows-x64-Setup.exe"))
                    throw new Exception("Wrong architecture must be blocked before installation.");
                Check(mismatch, "wrong-architecture-" + (int)(scale * 100), output);
            }
            using (var unsupported = new SetupWindow("x64", "AMD64", 20348)) {
                ShowScaled(unsupported, scale);
                if (Field<Button>(unsupported, "install").Enabled || Field<Button>(unsupported, "install").Visible
                    || unsupported.AcceptButton != Field<Button>(unsupported, "close")
                    || !Field<Label>(unsupported, "status").Text.Contains("Windows 11"))
                    throw new Exception("An unsupported Windows version must be blocked before installation.");
                Check(unsupported, "unsupported-windows-" + (int)(scale * 100), output);
            }
            using (var form = new SetupWindow("x64", "AMD64", Windows11Build)) {
                ShowScaled(form, scale);
                if (!Field<Button>(form, "install").Enabled || !Field<Button>(form, "install").Visible
                    || form.AcceptButton != Field<Button>(form, "install"))
                    throw new Exception("A supported Windows version must offer installation.");
                string suffix = "-" + (int)(scale * 100);
                Check(form, "initial" + suffix, output);
                form.PreviewBusy();
                Check(form, "busy" + suffix, output);
                Field<Button>(form, "install").Enabled = true;
                Field<Button>(form, "close").Enabled = true;
                Field<Label>(form, "heading").Text = "WHAT is installed";
                Field<Label>(form, "status").Text = "Ready. You can open WHAT or close this installer.";
                var details = Field<TextBox>(form, "details");
                details.Text = "Installed location:\r\nC:\\Program Files\\WindowsApps\\WHAT.Desktop_0.0.1.9_x64__z2ppg34bp4p5w";
                details.Visible = true;
                Field<Button>(form, "launch").Visible = true;
                Field<Button>(form, "install").Visible = false;
                Field<Button>(form, "close").Text = "Close";
                Field<SetupProgress>(form, "progress").Animating = false;
                Field<SetupProgress>(form, "progress").Completed = true;
                Check(form, "complete" + suffix, output);
                var progress = Field<SetupProgress>(form, "progress");
                using (var image = new Bitmap(progress.Width, progress.Height)) {
                    progress.DrawToBitmap(image, new Rectangle(Point.Empty, image.Size));
                    if (image.GetPixel(0, 0).ToArgb() != SetupTheme.Brand.ToArgb()
                        || image.GetPixel(image.Width - 1, image.Height - 1).ToArgb() != SetupTheme.Brand.ToArgb())
                        throw new Exception("Completed progress must fill its square track in the logo blue.");
                }
                Field<Label>(form, "heading").Text = "Installation did not complete";
                Field<Label>(form, "status").Text = "See the details below. You can retry after resolving the error.";
                Field<Button>(form, "launch").Visible = false;
                Field<Button>(form, "install").Visible = true;
                Field<Button>(form, "install").Text = "Retry";
                Field<Button>(form, "copy").Visible = true;
                Field<SetupProgress>(form, "progress").Visible = false;
                details.Text = "Windows installation failed.\r\nLog: C:\\Users\\Example\\AppData\\Local\\WHAT\\installer\\example\\install.log";
                Check(form, "error" + suffix, output);
            }
        }
    }
}
