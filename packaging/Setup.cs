using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Drawing;
using System.IO;
using System.IO.Compression;
using System.Xml;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text;
using System.Threading.Tasks;
using System.Windows.Forms;

// The package owns installation, updates and uninstall. This wizard only stages
// its embedded payload and drives the explicit first-install trust flow.
internal static class SetupProgram
{
    internal static readonly string[] Payloads = { "WHAT-Notifications.msix", "WHAT.cer", "Install-Notifications.ps1", "Trust-Certificate.ps1" };
    internal static Stream Resource(string name) {
        var stream = Assembly.GetExecutingAssembly().GetManifestResourceStream(name);
        if (stream == null) throw new InvalidDataException("Missing installer resource: " + name);
        return stream;
    }
    internal static void VerifyPayloads() {
        var expected = new Dictionary<string, string>(StringComparer.Ordinal);
        using (var reader = new StreamReader(Resource("PayloadHashes.txt"))) {
            string line;
            while ((line = reader.ReadLine()) != null) {
                int split = line.IndexOf(' ');
                if (split != 64) throw new InvalidDataException("Invalid payload index.");
                expected.Add(line.Substring(split + 1), line.Substring(0, split));
            }
        }
        if (expected.Count != Payloads.Length) throw new InvalidDataException("Incomplete payload index.");
        foreach (var name in Payloads) using (var stream = Resource(name)) using (var sha = SHA256.Create()) {
            var actual = BitConverter.ToString(sha.ComputeHash(stream)).Replace("-", "");
            if (!expected.ContainsKey(name) || !String.Equals(actual, expected[name], StringComparison.OrdinalIgnoreCase))
                throw new InvalidDataException("Installer payload is damaged: " + name);
        }
    }
    internal static void PruneLogs(string root, string current) {
        var parent = new DirectoryInfo(Path.GetFullPath(root));
        if (!parent.Exists || (parent.Attributes & FileAttributes.ReparsePoint) != 0) return;
        var history = new List<DirectoryInfo>();
        foreach (var folder in parent.GetDirectories()) {
            Guid id;
            if (Guid.TryParseExact(folder.Name, "N", out id)
                && (folder.Attributes & FileAttributes.ReparsePoint) == 0
                && !String.Equals(folder.FullName, Path.GetFullPath(current), StringComparison.OrdinalIgnoreCase))
                history.Add(folder);
        }
        history.Sort((a, b) => b.LastWriteTimeUtc.CompareTo(a.LastWriteTimeUtc));
        var allowed = new HashSet<string>(StringComparer.OrdinalIgnoreCase) {
            "install.log", "inspection.txt", "inspection.txt.location", "family.txt", "family.txt.location"
        };
        for (int index = 4; index < history.Count; ++index) {
            var folder = history[index];
            try {
                // Only our completed, flat log folders are eligible. Never follow
                // junctions or remove payloads belonging to another running Setup.
                if (folder.Parent.FullName != parent.FullName || folder.GetDirectories().Length != 0) continue;
                var files = folder.GetFiles();
                bool safe = true;
                foreach (var file in files)
                    safe &= allowed.Contains(file.Name) && (file.Attributes & FileAttributes.ReparsePoint) == 0;
                if (!safe) continue;
                foreach (var file in files) using (file.Open(FileMode.Open, FileAccess.ReadWrite, FileShare.None)) { }
                foreach (var file in files) file.Delete();
                folder.Delete();
            } catch (IOException) { } catch (UnauthorizedAccessException) { }
        }
    }

    internal static string PackageArchitecture() {
        // Read the embedded manifest, so the label cannot disagree with the payload.
        using (var stream = Resource("WHAT-Notifications.msix")) return PackageArchitecture(stream);
    }
    internal static string PackageArchitecture(Stream stream) {
        using (var archive = new ZipArchive(stream, ZipArchiveMode.Read, true)) {
            var entry = archive.GetEntry("AppxManifest.xml");
            if (entry == null || entry.Length > 1024 * 1024)
                throw new InvalidDataException("Package manifest missing or too large.");
            var manifest = new XmlDocument { XmlResolver = null };
            using (var input = entry.Open()) manifest.Load(input);
            var identity = manifest.SelectSingleNode("/*[local-name()='Package']/*[local-name()='Identity']");
            var attribute = identity == null ? null : identity.Attributes["ProcessorArchitecture"];
            string architecture = attribute == null ? "" : attribute.Value;
            if (architecture != "x64" && architecture != "arm64")
                throw new InvalidDataException("Unsupported WHAT package architecture.");
            return architecture;
        }
    }
    internal static string NativeArchitecture() {
        return Environment.GetEnvironmentVariable("PROCESSOR_ARCHITEW6432")
            ?? Environment.GetEnvironmentVariable("PROCESSOR_ARCHITECTURE") ?? "";
    }
    internal static string ArchitectureIssue(string package, string native) {
        return ArchitectureIssue(package, native, Environment.OSVersion.Version.Build);
    }
    internal static string ArchitectureIssue(string package, string native, int windowsBuild) {
        if (package == "arm64" && !String.Equals(native, "ARM64", StringComparison.OrdinalIgnoreCase))
            return "This installer is for Windows on ARM. For an Intel or AMD PC, use the Windows-x64-Setup.exe download.";
        if (!String.Equals(native, "AMD64", StringComparison.OrdinalIgnoreCase)
            && !String.Equals(native, "ARM64", StringComparison.OrdinalIgnoreCase))
            return "WHAT requires 64-bit Windows.";
        if (windowsBuild < (package == "arm64" ? 22621 : 22000))
            return package == "arm64" ? "Native ARM64 WHAT requires Windows 11 22H2 or newer."
                : "WHAT requires Windows 11 or newer.";
        return "";
    }

    private delegate bool WindowCallback(IntPtr window, IntPtr parameter);
    [DllImport("user32.dll")] private static extern bool EnumWindows(WindowCallback callback, IntPtr parameter);
    [DllImport("user32.dll")] private static extern uint GetWindowThreadProcessId(IntPtr window, out uint process);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] private static extern IntPtr GetProp(IntPtr window, string name);
    [DllImport("user32.dll")] private static extern bool PostThreadMessage(uint thread, uint message, UIntPtr wParam, IntPtr lParam);

    internal static bool RequestQuit(int processId) {
        bool requested = false;
        EnumWindows((window, parameter) => {
            uint owner;
            uint thread = GetWindowThreadProcessId(window, out owner);
            if (owner != (uint)processId || GetProp(window, "WHAT.MainWindow") == IntPtr.Zero)
                return true;
            // WM_QUIT lets Qt finish its normal settings and worker shutdown.
            requested = PostThreadMessage(thread, 0x0012, UIntPtr.Zero, IntPtr.Zero);
            return false;
        }, IntPtr.Zero);
        return requested;
    }

    internal static ProcessStartInfo PowerShellStartInfo(string script, string directory) {
        // Encoded commands otherwise serialize progress records as CLIXML when
        // redirected. Keep errors as text and omit module-initialization progress.
        script = "$ProgressPreference='SilentlyContinue'; " + script;
        return new ProcessStartInfo {
            FileName = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.System), "WindowsPowerShell", "v1.0", "powershell.exe"),
            Arguments = "-NoLogo -NoProfile -NonInteractive -OutputFormat Text -ExecutionPolicy Bypass -EncodedCommand "
                + Convert.ToBase64String(Encoding.Unicode.GetBytes(script)),
            UseShellExecute = false, CreateNoWindow = true, RedirectStandardOutput = true,
            RedirectStandardError = true, WorkingDirectory = directory
        };
    }
    [STAThread]
    private static int Main(string[] args) {
        // Build verification only: no UI, extraction, certificate trust or install.
        if (args.Length == 2 && args[0] == "--verify-payload") {
            try { VerifyPayloads(); return PackageArchitecture() == args[1] ? 0 : 1; } catch { return 1; }
        }
        Application.EnableVisualStyles();
        if (args.Length == 2 && (args[0] == "--render-preview" || args[0] == "--render-preview-busy")) {
            using (var preview = new SetupWindow()) {
                preview.ShowInTaskbar = false;
                preview.StartPosition = FormStartPosition.Manual;
                preview.Location = new Point(-32000, -32000);
                preview.Show(); Application.DoEvents();
                if (args[0] == "--render-preview-busy") preview.PreviewBusy();
                Application.DoEvents();
                using (var bitmap = new Bitmap(preview.Width, preview.Height)) {
                    preview.DrawToBitmap(bitmap, new Rectangle(0, 0, bitmap.Width, bitmap.Height));
                    bitmap.Save(args[1], System.Drawing.Imaging.ImageFormat.Png);
                }
            }
            return 0;
        }
        Application.SetCompatibleTextRenderingDefault(false);
        using (var window = new SetupWindow()) Application.Run(window);
        return 0;
    }
}

// Mirrors the default blue theme in qml/controls/Theme.qml. Qt tests compare
// the evaluated colours; installer layout tests check spacing and font sizes.
internal static class SetupTheme {
    internal static readonly Color Canvas = Color.FromArgb(0x10, 0x15, 0x1e);
    internal static readonly Color Inset = Color.FromArgb(0x0d, 0x14, 0x1d);
    internal static readonly Color Button = Color.FromArgb(0x20, 0x2c, 0x3e);
    internal static readonly Color Hover = Color.FromArgb(0x27, 0x3c, 0x4c);
    internal static readonly Color Pressed = Color.FromArgb(0x34, 0x55, 0x6a);
    internal static readonly Color Outline = Color.FromArgb(0x34, 0x43, 0x5a);
    internal static readonly Color Ink = Color.FromArgb(0xe7, 0xed, 0xf6);
    internal static readonly Color Muted = Color.FromArgb(0x8d, 0x9d, 0xb2);
    internal static readonly Color Brand = Color.FromArgb(0x1e, 0xb9, 0xe8);
    internal static readonly Color Accent = Color.FromArgb(0x66, 0xca, 0xff);
    internal static readonly Color AccentHover = Color.FromArgb(0x9c, 0xdd, 0xff);
    internal static readonly Color AccentText = Color.FromArgb(0x10, 0x27, 0x38);
    internal const int SpaceXs = 4, SpaceS = 8, SpaceM = 12, SpaceL = 16, SpaceXl = 24;
    internal const int ControlHeight = 36, FontBody = 13, FontHeading = 20, FontTitle = 28;

    internal static Font CreateFont(int pixels, bool semibold) {
        // QML sizes are logical pixels; WinForms fonts use points at 96 DPI.
        return new Font(semibold ? "Segoe UI Semibold" : "Segoe UI", pixels * 72f / 96f);
    }

    internal static void ConfigureWindow(Form form) {
        form.BackColor = Canvas;
        form.ForeColor = Ink;
        form.Font = CreateFont(FontBody, false);
    }

    internal static void ConfigureButton(System.Windows.Forms.Button button, bool primary) {
        button.AutoSize = true;
        button.AutoSizeMode = AutoSizeMode.GrowAndShrink;
        button.MinimumSize = new Size(112, ControlHeight);
        // MinimumSize supplies the vertical space; native button padding adds
        // border-dependent height and can make primary/secondary actions differ.
        button.Padding = new Padding(SpaceM, 0, SpaceM, 0);
        button.Margin = new Padding(SpaceS, 0, 0, 0);
        button.Font = CreateFont(FontBody, true);
        button.FlatStyle = FlatStyle.Flat;
        button.UseVisualStyleBackColor = false;
        button.BackColor = primary ? Accent : Button;
        button.ForeColor = primary ? AccentText : Ink;
        button.FlatAppearance.BorderSize = primary ? 0 : 1;
        button.FlatAppearance.BorderColor = Outline;
        button.FlatAppearance.MouseOverBackColor = primary ? AccentHover : Hover;
        button.FlatAppearance.MouseDownBackColor = primary ? Brand : Pressed;
    }
}

internal sealed class SetupWindow : Form
{
    readonly Label heading = new Label(), description = new Label(), status = new Label();
    readonly Button install = new Button(), close = new Button(), copy = new Button(), launch = new Button();
    readonly SetupProgress progress = new SetupProgress();
    readonly TextBox details = new TextBox();
    readonly FlowLayoutPanel actions = new FlowLayoutPanel();
    bool installing;
    string family = "", logPath = "", failure = "", expectedInstalledVersion = "none", installedLocation = "";
    bool allowDowngrade;
    public SetupWindow() : this(SetupProgram.PackageArchitecture(), SetupProgram.NativeArchitecture()) { }
    internal SetupWindow(string architecture, string nativeArchitecture) {
        string architectureLabel = architecture == "arm64" ? "ARM64" : "x64";
        Text = "WHAT Setup - " + architectureLabel;
        SuspendLayout();
        AutoScaleDimensions = new SizeF(96F, 96F);
        AutoScaleMode = AutoScaleMode.Dpi;
        ClientSize = new Size(640, 400);
        MinimumSize = new Size(640, 0);
        FormBorderStyle = FormBorderStyle.FixedDialog;
        MaximizeBox = false;
        StartPosition = FormStartPosition.CenterScreen;
        SetupTheme.ConfigureWindow(this);
        using (var stream = SetupProgram.Resource("app.ico")) Icon = new Icon(stream);

        var layout = new TableLayoutPanel {
            Dock = DockStyle.Top, AutoSize = true, AutoSizeMode = AutoSizeMode.GrowAndShrink, ColumnCount = 1,
            Padding = new Padding(SetupTheme.SpaceXl), BackColor = BackColor
        };
        layout.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
        Controls.Add(layout);
        // The details row appears after installation. Size from wrapped content
        // so every state keeps its buttons and bottom padding inside the window.
        layout.SizeChanged += delegate { ClientSize = new Size(ClientSize.Width, layout.Height); };
        var header = new TableLayoutPanel {
            AutoSize = true, Dock = DockStyle.Fill, ColumnCount = 2,
            Margin = new Padding(0, 0, 0, SetupTheme.SpaceXl)
        };
        header.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 80));
        header.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
        var picture = new PictureBox {
            Size = new Size(64, 72), SizeMode = PictureBoxSizeMode.Zoom,
            Margin = new Padding(0, 0, SetupTheme.SpaceL, 0)
        };
        using (var stream = SetupProgram.Resource("app.png"))
        using (var source = Image.FromStream(stream)) picture.Image = new Bitmap(source);
        header.Controls.Add(picture, 0, 0);
        header.SetRowSpan(picture, 2);
        heading.Text = "Install WHAT for " + architectureLabel;
        heading.Font = SetupTheme.CreateFont(SetupTheme.FontTitle, true);
        heading.AutoSize = true;
        heading.Dock = DockStyle.Fill;
        heading.Margin = new Padding(0, 0, 0, SetupTheme.SpaceXs);
        header.Controls.Add(heading, 1, 0);
        description.Text = "Windows Home Assistant Toolkit\r\nDeveloped by Chuffnugget  |  Free and open source";
        description.AutoSize = true;
        description.Dock = DockStyle.Fill;
        description.Margin = new Padding(0);
        description.ForeColor = SetupTheme.Muted;
        header.Controls.Add(description, 1, 1);
        layout.Controls.Add(header);

        layout.Controls.Add(new Label {
            AutoSize = true, Dock = DockStyle.Fill,
            Margin = new Padding(0, 0, 0, SetupTheme.SpaceL),
            Text = "Windows manages the installation folder. Your WHAT settings and sensor selections are kept when updating or reinstalling."
        });
        layout.Controls.Add(new Label {
            AutoSize = true, Dock = DockStyle.Fill,
            Margin = new Padding(0, 0, 0, SetupTheme.SpaceXl),
            Text = "Trusting the included signing certificate requires administrator approval unless it is already trusted."
        });
        status.Text = "Ready to install";
        status.AutoSize = true;
        status.Dock = DockStyle.Fill;
        status.Margin = new Padding(0, 0, 0, SetupTheme.SpaceL);
        layout.Controls.Add(status);
        progress.Height = SetupTheme.SpaceS;
        progress.Dock = DockStyle.Fill;
        progress.Margin = new Padding(0, 0, 0, SetupTheme.SpaceL);
        progress.Visible = false;
        layout.Controls.Add(progress);
        details.Multiline = true;
        details.ReadOnly = true;
        details.ScrollBars = ScrollBars.Vertical;
        details.BackColor = SetupTheme.Inset;
        details.ForeColor = SetupTheme.Ink;
        details.BorderStyle = BorderStyle.None;
        details.Height = 112;
        details.Dock = DockStyle.Fill;
        details.Visible = false;
        details.Margin = new Padding(0, 0, 0, SetupTheme.SpaceL);
        layout.Controls.Add(details);
        actions.Dock = DockStyle.Fill;
        actions.AutoSize = true;
        actions.FlowDirection = FlowDirection.RightToLeft;
        actions.WrapContents = true;
        actions.Margin = new Padding(0);
        layout.Controls.Add(actions);
        ConfigureButton(install, "Install", delegate { BeginInstallation(); });
        ConfigureButton(close, "Cancel", delegate { Close(); });
        ConfigureButton(copy, "Copy error", delegate {
            Clipboard.SetText(failure + "\r\nLog: " + logPath + "\r\n" + details.Text);
        });
        copy.Visible = false;
        ConfigureButton(launch, "Open app", delegate { LaunchApp(); });
        launch.Visible = false;
        AcceptButton = install;
        CancelButton = close;
        string compatibility = SetupProgram.ArchitectureIssue(architecture, nativeArchitecture);
        if (compatibility.Length > 0) {
            heading.Text = "This installer cannot run on this PC";
            status.Text = compatibility;
            install.Enabled = false;
            install.Visible = false;
            close.Text = "Close";
            AcceptButton = close;
        }
        ResumeLayout(true);
        FormClosing += delegate(object sender, FormClosingEventArgs e) {
            if (installing) e.Cancel = true;
        };
    }

    internal void PreviewBusy() {
        progress.Visible = true;
        progress.Animating = true;
        status.Text = "Installing...";
        install.Enabled = close.Enabled = false;
    }

    void ConfigureButton(Button button, string text, EventHandler click) {
        button.Text = text;
        SetupTheme.ConfigureButton(button, button == install || button == launch);
        button.Click += click;
        actions.Controls.Add(button);
    }

    static string QuotePowerShell(string value) { return "'" + value.Replace("'", "''") + "'"; }
    async void BeginInstallation() {
        if (installing) return;
        installing = true; install.Enabled = close.Enabled = false; copy.Visible = false;
        progress.Visible = true; progress.Animating = true; status.Text = "Installing... respond to the Windows approval prompt if shown.";
        details.Text = ""; details.Visible = false;

        try {
            status.Text = "Checking for an existing WHAT installation...";
            string[] plan = (await Task.Run(() => InstallPayload(true))).Split(new[] { '\r', '\n' }, StringSplitOptions.RemoveEmptyEntries);
            if (plan.Length != 3) throw new InvalidDataException("Could not determine the existing installation. Nothing was installed.");
            string mode = plan[0], previous = plan[1], target = plan[2];
            expectedInstalledVersion = previous;
            if (mode == "publisher-change") throw new IOException("WHAT " + previous + " is installed under a different publisher. Back up your profile, uninstall the old WHAT entry through Windows Settings, then run this installer. This is a one-time publisher migration, not an overwrite.");
            if (mode == "ambiguous") throw new IOException("Multiple matching WHAT installations were found. Resolve these through Windows Settings before continuing.");
            if (mode != "install" && mode != "update" && mode != "reinstall" && mode != "downgrade") throw new InvalidDataException("Unknown installation action.");
            allowDowngrade = false;
            if (mode != "install") {
                string action = mode == "downgrade" ? "Downgrade" : mode == "update" ? "Update" : "Reinstall";
                string message = "WHAT " + previous + " is already installed.\r\n\r\n" + action + " using version " + target + "? Your settings and sensor selections will be kept. No uninstall or profile reset will be performed.";
                if (installedLocation.Length > 0) message += "\r\n\r\nCurrent installation: " + installedLocation;
                if (mode == "downgrade") message += "\r\n\r\nOlder versions may not understand settings saved by newer versions. Back up your profile before continuing.";
                if (SetupMessage.Show(this, message, action + " WHAT?", true) != DialogResult.Yes) {
                    status.Text = "Installation cancelled. Nothing was changed.";
                    return;
                }
            }
            allowDowngrade = mode == "downgrade";
            if (!await HandleRunningApp()) { status.Text = "Installation cancelled. Nothing was installed."; return; }
            status.Text = "Installing... respond to the Windows approval prompt if shown.";
            string installedFamily = await Task.Run(() => InstallPayload(false));
            family = installedFamily; heading.Text = "WHAT is installed";
            status.Text = "Ready. You can open WHAT or close this installer.";
            details.Text = "Installed location:\r\n" + installedLocation;
            details.Visible = true;
            PerformLayout();
            launch.Visible = family.Length > 0; install.Visible = false;
        } catch (Exception error) {
            failure = error.Message; heading.Text = "Installation did not complete";
            status.Text = "See the details below. You can retry after resolving the error.";
            details.Visible = true; PerformLayout();
            details.AppendText("\r\n" + failure + "\r\nLog: " + logPath); copy.Visible = true; install.Text = "Retry";
        } finally {
            installing = false; install.Enabled = close.Enabled = true; close.Text = "Close";
            progress.Animating = false; progress.Completed = family.Length > 0; progress.Visible = progress.Completed;
        }
    }
    async Task<bool> HandleRunningApp() {
        var running = new List<Process>();
        try {
            int session = Process.GetCurrentProcess().SessionId;
            foreach (var process in Process.GetProcessesByName("WHAT")) {
                try {
                    if (process.SessionId == session && !process.HasExited) { var handle = process.Handle; running.Add(process); }
                    else process.Dispose();
                } catch { process.Dispose(); }
            }
            if (running.Count == 0) return true;
            using (var prompt = new RunningAppPrompt()) {
                var choice = prompt.ShowDialog(this);
                if (choice == DialogResult.Ignore) return true;
                if (choice != DialogResult.OK) return false;
            }
            await Task.Run(() => {
                foreach (var process in running) {
                    if (process.HasExited) continue;
                    SetupProgram.RequestQuit(process.Id);
                    if (process.WaitForExit(10000)) continue;
                    // An unresponsive app must not leave Setup waiting forever.
                    if (!process.HasExited) process.Kill();
                    if (!process.WaitForExit(5000))
                        throw new IOException("WHAT did not close. Quit it from the tray and retry.");
                }
            });
            return true;
        } catch (Exception error) {
            SetupMessage.Show(this, error.Message, "Could not close WHAT", false);
            return false;
        } finally { foreach (var process in running) process.Dispose(); }
    }
    string InstallPayload(bool inspectOnly) {
        SetupProgram.VerifyPayloads();
        string folder = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "WHAT", "installer", Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(folder); logPath = Path.Combine(folder, "install.log");
        try {
            foreach (var name in SetupProgram.Payloads) using (var input = SetupProgram.Resource(name)) using (var output = File.Create(Path.Combine(folder, name))) input.CopyTo(output);
            // The wizard stays unelevated. Only certificate import requests UAC,
            // so package registration targets the user who launched Setup.
            string script = "$ErrorActionPreference='Stop'; try { & " + QuotePowerShell(Path.Combine(folder, "Install-Notifications.ps1")) + " -FromSetup -FamilyOutputPath " + QuotePowerShell(Path.Combine(folder, "family.txt")) + "; exit 0 } catch { [Console]::Error.WriteLine($_.Exception.ToString()); exit 1 }";
            if (inspectOnly) script = "$ErrorActionPreference='Stop'; try { & " + QuotePowerShell(Path.Combine(folder, "Install-Notifications.ps1")) + " -InspectOnly -InspectionOutputPath " + QuotePowerShell(Path.Combine(folder, "inspection.txt")) + "; exit 0 } catch { [Console]::Error.WriteLine($_.Exception.ToString()); exit 1 }";
            else script = script.Replace(" -FromSetup ", " -FromSetup -ExpectedInstalledVersion " + QuotePowerShell(expectedInstalledVersion) + (allowDowngrade ? " -AllowDowngrade " : " "));
            var info = SetupProgram.PowerShellStartInfo(script, folder);
            using (var log = new StreamWriter(logPath, false, Encoding.UTF8)) using (var process = new Process { StartInfo = info }) {
                object gate = new object();
                DataReceivedEventHandler receive = delegate(object sender, DataReceivedEventArgs e) {
                    if (e.Data == null) return;
                    lock (gate) { log.WriteLine(e.Data); log.Flush(); }
                    BeginInvoke((Action)(() => details.AppendText(e.Data + "\r\n")));
                };
                process.OutputDataReceived += receive; process.ErrorDataReceived += receive;
                if (!process.Start()) throw new IOException("Could not start Windows package installation.");
                process.BeginOutputReadLine(); process.BeginErrorReadLine(); process.WaitForExit();
                if (process.ExitCode != 0) throw new IOException("The installer could not complete. See the details above for the reason.");
            }
            if (inspectOnly) {
                installedLocation = File.ReadAllText(Path.Combine(folder, "inspection.txt.location")).Trim();
                return File.ReadAllText(Path.Combine(folder, "inspection.txt"));
            }
            installedLocation = File.ReadAllText(Path.Combine(folder, "family.txt.location")).Trim();
            var result = File.ReadAllText(Path.Combine(folder, "family.txt")).Trim();
            if (!System.Text.RegularExpressions.Regex.IsMatch(result, @"\AWHAT\.(Desktop|Development)_[a-z0-9]+\z")) throw new InvalidDataException("Unexpected installed package identity.");
            return result;
        } finally {
            // Keep diagnostic text, but never retain a duplicate package payload.
            foreach (var name in SetupProgram.Payloads) { try { File.Delete(Path.Combine(folder, name)); } catch { } }
            try { SetupProgram.PruneLogs(Path.GetDirectoryName(folder), folder); }
            catch (IOException) { } catch (UnauthorizedAccessException) { }
        }
    }
    void LaunchApp() {
        try { Process.Start(new ProcessStartInfo("explorer.exe", "shell:AppsFolder\\" + family + "!App") { UseShellExecute = true }); Close(); }
        catch (Exception error) { SetupMessage.Show(this, error.Message, "Could not open WHAT", false); }
    }
}

internal sealed class SetupProgress : Control {
    readonly Timer timer = new Timer();
    float phase;
    bool completed;

    public SetupProgress() {
        SetStyle(ControlStyles.UserPaint | ControlStyles.AllPaintingInWmPaint | ControlStyles.OptimizedDoubleBuffer, true);
        timer.Interval = 25;
        timer.Tick += delegate {
            phase = (phase + 0.015f) % 1f;
            Invalidate();
        };
    }

    public bool Animating {
        set {
            timer.Enabled = value;
            if (value) completed = false;
            Invalidate();
        }
    }

    public bool Completed {
        get { return completed; }
        set { completed = value; Invalidate(); }
    }

    protected override void OnPaint(PaintEventArgs e) {
        // Square ends and the same logo blue as WHAT's update progress bar.
        e.Graphics.Clear(SetupTheme.Outline);
        if (!timer.Enabled && !completed) return;
        float length = completed ? Width : Width * 0.24f;
        float start = completed ? 0 : (Width - length) * (0.5f - 0.5f * (float)Math.Cos(phase * Math.PI * 2));
        using (var fill = new SolidBrush(SetupTheme.Brand))
            e.Graphics.FillRectangle(fill, start, 0, length, Height);
    }

    protected override void Dispose(bool disposing) {
        if (disposing) timer.Dispose();
        base.Dispose(disposing);
    }
}

internal static class SetupMessage {
    internal static void Configure(Form form, string title, string message, string[] labels, DialogResult[] results) {
        form.SuspendLayout();
        form.Text = title;
        form.AutoScaleDimensions = new SizeF(96, 96);
        form.AutoScaleMode = AutoScaleMode.Dpi;
        form.ClientSize = new Size(560, 180);
        form.MinimumSize = new Size(560, 0);
        form.FormBorderStyle = FormBorderStyle.FixedDialog;
        form.MaximizeBox = form.MinimizeBox = false;
        form.ShowInTaskbar = false;
        form.StartPosition = FormStartPosition.CenterParent;
        SetupTheme.ConfigureWindow(form);
        using (var stream = SetupProgram.Resource("app.ico")) form.Icon = new Icon(stream);
        var layout = new TableLayoutPanel {
            Dock = DockStyle.Top, AutoSize = true, AutoSizeMode = AutoSizeMode.GrowAndShrink, ColumnCount = 1,
            Padding = new Padding(SetupTheme.SpaceXl)
        };
        layout.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
        form.Controls.Add(layout);
        // Include wrapped text, buttons and bottom padding at every display scale.
        layout.SizeChanged += delegate { form.ClientSize = new Size(form.ClientSize.Width, layout.Height); };
        layout.Controls.Add(new Label {
            Text = title, ForeColor = SetupTheme.Ink,
            Font = SetupTheme.CreateFont(SetupTheme.FontHeading, true),
            AutoSize = true, Dock = DockStyle.Fill,
            Margin = new Padding(0, 0, 0, SetupTheme.SpaceL)
        });
        layout.Controls.Add(new Label {
            Text = message, AutoSize = true, Dock = DockStyle.Fill,
            Margin = new Padding(0, 0, 0, SetupTheme.SpaceXl)
        });
        var actions = new FlowLayoutPanel {
            AutoSize = true, Dock = DockStyle.Fill, FlowDirection = FlowDirection.RightToLeft,
            WrapContents = true, Margin = new Padding(0)
        };
        layout.Controls.Add(actions);
        for (int i = labels.Length - 1; i >= 0; --i) {
            var button = new Button { Text = labels[i], DialogResult = results[i] };
            SetupTheme.ConfigureButton(button, i == 0);
            actions.Controls.Add(button);
            // Keep Cancel as the default for confirmations, including close-app prompts.
            if (i == labels.Length - 1) {
                form.AcceptButton = button;
                form.CancelButton = button;
            }
        }
        form.ResumeLayout(true);
    }

    internal static DialogResult Show(IWin32Window owner, string message, string title, bool confirm) {
        using (var form = new Form()) {
            Configure(form, title, message, confirm ? new[] { "Continue", "Cancel" } : new[] { "OK" },
                confirm ? new[] { DialogResult.Yes, DialogResult.No } : new[] { DialogResult.OK });
            return form.ShowDialog(owner);
        }
    }
}

internal sealed class RunningAppPrompt : Form {
    public RunningAppPrompt() {
        SetupMessage.Configure(this, "WHAT is running", "Close WHAT to install the new version?\r\n\r\nClose app ends the running monitor. Ignore leaves it running and attempts installation; Windows may reject it.", new[] { "Close app", "Ignore", "Cancel" }, new[] { DialogResult.OK, DialogResult.Ignore, DialogResult.Cancel });
    }
}
