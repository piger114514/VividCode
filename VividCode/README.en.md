# vividcode — Chinese / English interface

vividcode is a Windows x64 desktop tool for exploring source files, symbols and code relationships. The desktop interface supports Simplified Chinese and English; command-line output and analyzer diagnostics remain in their original language.

## Install and open a project

1. Run `vividcode-Setup-0.1.4-x64.exe` and select the installation folder.
2. Open a new PowerShell window in your source project and run `vividcode -g`.
3. Open `.vividcode/vividcode-viewer.exe` in that project. You can also launch `runtime/vividcode-viewer.exe` from the installation folder and click **Open Project...**.

For an existing installation, keeping the same folder upgrades it. Selecting a new empty folder migrates the program, settings and caches, verifies them, and removes the previous installation. Close vividcode before upgrading or moving it.

## Runtime requirements

The package includes Qt, the app-local Microsoft Visual C++ runtime, and the C++, C#, Python, JavaScript/TypeScript and Java analyzer components. Language runtimes are installed separately on your computer:

| Language | Required local tools |
| --- | --- |
| C/C++ | Python 3.12+ x64, LLVM/libclang, and a real `compile_commands.json` |
| C# / Unity | The .NET SDK and workloads required by the project; `.sln` or `.csproj` |
| Python | Python 3.12+ x64 |
| JavaScript / TypeScript | Node.js 22 or 24 x64 |
| Java | A full JDK 17+ |

Right-click the graph and choose **Analysis Environment...** to set local tool paths. Empty paths are detected from PATH. No runtimes are downloaded automatically.

## Everyday controls

- **Open Project...** opens a source folder; **Refresh Files** or F5 checks for changes.
- Filter the **Project Files** tree by file name or path.
- **Structure and Relations** lists indexed classes, functions and variables.
- Right-click nodes to **View Source**, **Show Details**, **Show Summary** or **Show Related Relations**.
- Right-click the canvas for **Show All Relations**, **Simplify All** and **Fit to Canvas**.
- Edges point from the referenced symbol to its referrer. Dashed edges indicate possible calls.

## Source preview

**View Source** opens a read-only preview. Choose **Semantic highlighting**, **Syntax highlighting**, or **Plain text** in its top-left selector. Indexed types, functions, variables, parameters and members use different colors; unindexed positions fall back to syntax rules. Source content is preserved. Refresh the project after source changes.

## AI explanations and Pi

Open **AI Settings**, enter your provider's Base URL, model and API key, then click **AI Analyze**. **Follow interface** generates explanations in the active interface language; **Chinese** and **English** can also be selected explicitly. Beginner, Intermediate and Professional explanation levels use separate caches. Switching language does not send requests; an uncached language profile requires generating explanations when you next click AI Analyze.

Source content is sent to the configured provider; API charges may apply. Click graph nodes to read saved explanations. Reading a card does not send a new request.

The Pi panel connects to an existing local Pi installation and uses its model, login and tools. Pi can modify the current project. Automatic graph and AI updates after a task are off by default.

## Command line

```text
vividcode -g     Refresh project
vividcode -sg    Refresh without file limits
vividcode -c     Set the global source file limit
vividcode -n     Clear old records, retaining the latest
vividcode -d     Clear this project's generated data and caches
vividcode -help  Show help
```

Type `stop` and press Enter in the same console to cancel generation. Force mode (`-sg`) still respects directory exclusions and may use substantial resources.

Select **Language / 语言** in the top toolbar to choose **简体中文** or **English**. Your selection is saved and takes effect when you reopen vividcode; current analysis and Pi sessions continue uninterrupted. English is the default for new settings. **Follow interface** in AI Settings uses the active interface language; an explicitly selected explanation language remains independent.

User source code, paths, project names, analyzer diagnostics, CLI output and third-party content retain their original language. The UI language preference is stored separately from AI settings in `ui-settings.json` under the vividcode installation home.
