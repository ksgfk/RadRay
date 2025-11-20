using System;
using System.Collections.Generic;
using System.IO;
using System.Text;
using Microsoft.Build.Framework;
using Microsoft.Build.Utilities;

public class CompileCommandsJson : Logger
{
    private StreamWriter sw;
    private bool firstEntry;
    private static readonly string[] SourceFileExtensions = new[]
    {
        ".c", ".cc", ".cp", ".cxx", ".cpp", ".c++", ".m", ".mm", ".ixx" // basic set
    };

    public override void Initialize(IEventSource eventSource)
    {
        string outputFilePath = String.IsNullOrEmpty(Parameters) ? "compile_commands.json" : Parameters;
        try
        {
            const bool append = false;
            Encoding utf8WithoutBom = new UTF8Encoding(false);
            sw = new StreamWriter(outputFilePath, append, utf8WithoutBom);
            firstEntry = true;
            sw.WriteLine("[");
        }
        catch (Exception ex)
        {
            Console.Error.WriteLine($"Failed to open {outputFilePath}: {ex.Message}\n{ex.StackTrace}");
        }
        eventSource.AnyEventRaised += EventSource_AnyEventRaised;
    }

    private void EventSource_AnyEventRaised(object sender, BuildEventArgs args)
    {
        if (!(args is TaskCommandLineEventArgs taskArgs)) return;
        string cmd = taskArgs.CommandLine;
        if (string.IsNullOrWhiteSpace(cmd)) return;

        try
        {
            if (!IsCompileCommand(cmd)) return;

            var arguments = SplitCommandLine(cmd);
            if (arguments == null || arguments.Count == 0) return;
            FixCompilerFirstArgument(arguments);
            FixDefineArguments(arguments);

            // Collect all source files from tokenized arguments (robust for multi-file cl invocations)
            var sourceFiles = new List<string>();
            for (int i = 0; i < arguments.Count; i++)
            {
                var a = arguments[i];
                if (LooksLikeSourceFile(a)) sourceFiles.Add(a);
            }
            if (sourceFiles.Count == 0) return; // nothing to emit

            // Normalize once
            var normalizedSourceFiles = new List<string>(sourceFiles.Count);
            for (int i = 0; i < sourceFiles.Count; i++) normalizedSourceFiles.Add(NormalizePath(sourceFiles[i]));

            string projectDir = null;
            if (!string.IsNullOrEmpty(taskArgs.ProjectFile))
            {
                try { projectDir = Path.GetDirectoryName(Path.GetFullPath(taskArgs.ProjectFile)); } catch { }
            }

            for (int i = 0; i < normalizedSourceFiles.Count; i++)
            {
                string sourceFile = normalizedSourceFiles[i];
                string sourceDir = null;
                try { sourceDir = Path.GetDirectoryName(sourceFile); } catch { sourceDir = Directory.GetCurrentDirectory(); }
                string commonDir = null;
                if (!string.IsNullOrEmpty(projectDir)) commonDir = GetCommonDirectory(projectDir, sourceDir);
                if (string.IsNullOrEmpty(commonDir)) commonDir = sourceDir; // fallback
                string relativeFile = GetRelativePath(commonDir, sourceFile);

                var argsForThisFile = FilterArgumentsForFile(arguments, normalizedSourceFiles, sourceFile);
                WriteEntry(commonDir, relativeFile, argsForThisFile);
            }
        }
        catch (Exception ex)
        {
            SafeWriteComment($"ERROR: {ex.Message.Replace("\r", " ").Replace("\n", " ")} ");
        }
    }

    private bool IsCompileCommand(string cmd)
    {
        string lower = cmd.ToLowerInvariant();
        if (!(lower.Contains("cl.exe") || lower.Contains("clang-cl.exe"))) return false;
        if (!(lower.Contains(" /c ") || lower.EndsWith(" /c") || lower.Contains(" -c ") || lower.EndsWith(" -c"))) return false;
        return true;
    }

    private string ExtractSourceFile(string cmd)
    {
        int i = cmd.Length - 1;
        while (i >= 0 && char.IsWhiteSpace(cmd[i])) i--;
        if (i < 0) return null;
        while (i >= 0)
        {
            string token;
            if (cmd[i] == '"')
            {
                int endQuote = i; i--; int startQuote = cmd.LastIndexOf('"', i); if (startQuote < 0) break;
                token = cmd.Substring(startQuote + 1, endQuote - startQuote - 1);
                i = startQuote - 1;
            }
            else
            {
                int end = i; while (i >= 0 && !char.IsWhiteSpace(cmd[i])) i--; int start = i + 1; token = cmd.Substring(start, end - start + 1);
            }
            if (LooksLikeSourceFile(token)) return NormalizePath(token);
            while (i >= 0 && char.IsWhiteSpace(cmd[i])) i--;
        }
        return null;
    }

    private List<string> ExtractSourceFiles(string cmd)
    {
        var files = new List<string>();
        int i = cmd.Length - 1;
        while (i >= 0 && char.IsWhiteSpace(cmd[i])) i--;
        if (i < 0) return files;
        bool collectedAny = false;
        while (i >= 0)
        {
            string token;
            if (cmd[i] == '"')
            {
                int endQuote = i; i--; int startQuote = cmd.LastIndexOf('"', i); if (startQuote < 0) break;
                token = cmd.Substring(startQuote + 1, endQuote - startQuote - 1);
                i = startQuote - 1;
            }
            else
            {
                int end = i; while (i >= 0 && !char.IsWhiteSpace(cmd[i])) i--; int start = i + 1; token = cmd.Substring(start, end - start + 1);
            }

            if (LooksLikeSourceFile(token))
            {
                files.Add(token);
                collectedAny = true;
            }
            else
            {
                // If we've already started collecting trailing source files, stop at first non-source token
                if (collectedAny) break;
            }

            while (i >= 0 && char.IsWhiteSpace(cmd[i])) i--;
        }
        files.Reverse();
        return files;
    }

    private string NormalizePath(string p)
    {
        try
        {
            if (p.IndexOfAny(new[] { '*', '?', '>' , '<', '|' }) >= 0) return p; // avoid Path.GetFullPath on obviously invalid patterns
            if (Path.IsPathRooted(p)) return Path.GetFullPath(p);
            return Path.GetFullPath(Path.Combine(Directory.GetCurrentDirectory(), p));
        }
        catch { return p; }
    }

    private bool LooksLikeSourceFile(string path)
    {
        if (string.IsNullOrEmpty(path)) return false;
        if (path[0] == '/' || path[0] == '-') return false; // exclude switches accidentally ending with source-like extensions
        string ext = Path.GetExtension(path).ToLowerInvariant();
        foreach (var s in SourceFileExtensions) if (ext == s) return true; return false;
    }

    private void WriteEntry(string directory, string file, IList<string> arguments)
    {
        if (sw == null) return;
        if (!firstEntry) sw.WriteLine(",");
        firstEntry = false;
        sw.Write("  {");
        sw.Write($"\"directory\": \"{EscapeJson(directory)}\", ");
        sw.Write($"\"file\": \"{EscapeJson(file)}\", ");
        sw.Write("\"arguments\": [");
        for (int i = 0; i < arguments.Count; i++) { if (i > 0) sw.Write(", "); sw.Write($"\"{EscapeJson(arguments[i])}\""); }
        sw.Write("]");
        sw.Write("}");
        sw.Flush();
    }

    private IList<string> FilterArgumentsForFile(IList<string> args, IList<string> allSourceFiles, string targetSource)
    {
        var allSet = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        for (int i = 0; i < allSourceFiles.Count; i++) allSet.Add(NormalizePath(allSourceFiles[i]));
        var result = new List<string>(args.Count);
        bool hasTarget = false;
        for (int i = 0; i < args.Count; i++)
        {
            string a = args[i];
            if (LooksLikeSourceFile(a))
            {
                string norm = NormalizePath(a);
                if (string.Equals(norm, targetSource, StringComparison.OrdinalIgnoreCase)) { result.Add(a); hasTarget = true; }
                else if (!allSet.Contains(norm)) { result.Add(a); }
                // else skip other source files
            }
            else
            {
                result.Add(a);
            }
        }
        if (!hasTarget) result.Add(targetSource);
        return result;
    }

    private void SafeWriteComment(string text)
    { if (sw == null) return; sw.WriteLine(); sw.WriteLine($"/* {EscapeJson(text)} */"); }

    private string EscapeJson(string s)
    {
        if (s == null) return ""; var sb = new StringBuilder(s.Length + 16);
        foreach (char c in s)
        {
            switch (c)
            {
                case '\\': sb.Append("\\\\"); break; case '"': sb.Append("\\\""); break; case '\n': sb.Append("\\n"); break; case '\r': sb.Append("\\r"); break; case '\t': sb.Append("\\t"); break; case '\b': sb.Append("\\b"); break; case '\f': sb.Append("\\f"); break;
                default: if (c < 0x20) sb.AppendFormat("\\u{0:X4}", (int)c); else sb.Append(c); break;
            }
        }
        return sb.ToString();
    }

    public override void Shutdown()
    {
        try
        {
            if (sw != null)
            { if (!firstEntry) sw.WriteLine(); sw.WriteLine("]"); sw.Flush(); sw.Close(); }
        }
        finally { base.Shutdown(); }
    }

    public static IList<string> SplitCommandLine(string commandLine)
    {
        var results = new List<string>();
        if (string.IsNullOrEmpty(commandLine)) return results;
        var current = new StringBuilder();
        bool inQuotes = false;
        int backslashCount = 0;
        for (int i = 0; i < commandLine.Length; i++)
        {
            char c = commandLine[i];
            if (c == '\\') { backslashCount++; continue; }
            if (c == '"')
            {
                if (backslashCount % 2 == 0)
                { if (backslashCount > 0) current.Append(new string('\\', backslashCount / 2)); inQuotes = !inQuotes; }
                else { if (backslashCount > 1) current.Append(new string('\\', backslashCount / 2)); current.Append('"'); }
                backslashCount = 0; continue;
            }
            if (backslashCount > 0) { current.Append(new string('\\', backslashCount)); backslashCount = 0; }
            if (char.IsWhiteSpace(c) && !inQuotes)
            { if (current.Length > 0) { results.Add(current.ToString()); current.Length = 0; } continue; }
            current.Append(c);
        }
        if (backslashCount > 0) current.Append(new string('\\', backslashCount));
        if (current.Length > 0) results.Add(current.ToString());
        return results;
    }

    private void FixCompilerFirstArgument(IList<string> args)
    {
        if (args == null || args.Count == 0) return;
        if (args[0].IndexOf(' ') >= 0) return;
        if (args[0].EndsWith(".exe", StringComparison.OrdinalIgnoreCase)) return;
        int exeIndex = -1;
        for (int i = 0; i < args.Count; i++) { if (args[i].EndsWith(".exe", StringComparison.OrdinalIgnoreCase)) { exeIndex = i; break; } }
        if (exeIndex > 0)
        {
            var sb = new StringBuilder();
            for (int i = 0; i <= exeIndex; i++) { if (i > 0) sb.Append(' '); sb.Append(args[i]); }
            string merged = sb.ToString();
            for (int i = exeIndex; i >= 1; i--) args.RemoveAt(i);
            args[0] = merged;
        }
    }

    private void FixDefineArguments(IList<string> args)
    {
        if (args == null || args.Count < 2) return;
        var merged = new List<string>(args.Count);
        for (int i = 0; i < args.Count; i++)
        {
            string a = args[i];
            if ((string.Equals(a, "/D", StringComparison.OrdinalIgnoreCase) || string.Equals(a, "-D", StringComparison.OrdinalIgnoreCase)) && i + 1 < args.Count)
            {
                string next = args[i + 1];
                if (!string.IsNullOrEmpty(next) && next[0] != '/' && next[0] != '-') { merged.Add(a + next); i++; continue; }
            }
            merged.Add(a);
        }
        if (merged.Count != args.Count) { args.Clear(); for (int i = 0; i < merged.Count; i++) args.Add(merged[i]); }
    }

    private static string GetRelativePath(string basePath, string path)
    {
        if (string.IsNullOrEmpty(basePath)) return path;
        try
        {
            basePath = Path.GetFullPath(basePath);
            path = Path.GetFullPath(path);
            if (!basePath.EndsWith(Path.DirectorySeparatorChar.ToString())) basePath += Path.DirectorySeparatorChar;
            var baseUri = new Uri(basePath, UriKind.Absolute);
            var pathUri = new Uri(path, UriKind.Absolute);
            var relUri = baseUri.MakeRelativeUri(pathUri);
            var rel = Uri.UnescapeDataString(relUri.ToString()).Replace('/', Path.DirectorySeparatorChar);
            return rel.Length == 0 ? "." : rel;
        }
        catch { return path; }
    }

    private static string GetCommonDirectory(string pathA, string pathB)
    {
        if (string.IsNullOrEmpty(pathA) || string.IsNullOrEmpty(pathB)) return null;
        try
        {
            pathA = Path.GetFullPath(pathA).TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar);
            pathB = Path.GetFullPath(pathB).TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar);
            var partsA = pathA.Split(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar);
            var partsB = pathB.Split(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar);
            int len = Math.Min(partsA.Length, partsB.Length);
            var sb = new StringBuilder();
            int i = 0;
            for (; i < len; i++)
            {
                if (!string.Equals(partsA[i], partsB[i], StringComparison.OrdinalIgnoreCase)) break;
                if (i == 0 && partsA[i].EndsWith(":")) // drive letter
                {
                    sb.Append(partsA[i]);
                }
                else
                {
                    if (sb.Length == 0) sb.Append(partsA[i]); else sb.Append(Path.DirectorySeparatorChar).Append(partsA[i]);
                }
            }
            if (sb.Length == 0) return null;
            return sb.ToString();
        }
        catch { return null; }
    }
}
