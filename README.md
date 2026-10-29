<h1><img src="assets/icon/png/dirsize-64.png" width="48" height="48" align="absmiddle" alt=""> DirectorySize</h1>

[![cpp](https://github.com/ali33/DirectorySize/actions/workflows/cpp.yml/badge.svg)](https://github.com/ali33/DirectorySize/actions/workflows/cpp.yml)

Find the biggest directories inside a selected directory.

## C++ version (Win32 API + Linux CLI)

A rewrite in C++17 by the same author: a native Win32 GUI, a console app for Windows and Linux, no .NET required,
multi-threaded scanning. Source, build instructions and design notes: **[cpp/README.md](cpp/README.md)**.
Download the latest build from the **[Releases page](https://github.com/ali33/DirectorySize/releases)**
(`dirsize-windows-x64.zip` with `dirsize-gui.exe` and `dirsize-cli.exe`, `dirsize-linux-x64.tar.gz` with `dirsize-cli`).

Measured on `C:\Windows\System32` (23,760 files, 1,953 directories, 9.63 GB; Windows 11, 16 logical cores, warm file cache,
3 runs each, Defender on):

| Tool | Time | Result |
|---|---|---|
| DirectorySize C++ (`-j 16`, NtQueryDirectoryFile) | 0.06 - 0.10 s | complete |
| DirectorySize C++ (`-j 1`) | 0.18 - 0.26 s | complete |
| PowerShell `Get-ChildItem -Recurse -File \| Measure-Object` | 2.0 - 2.2 s (2 runs) | complete |
| DirectorySize C# (this repo, with its premature "Finished" bug patched in the benchmark copy) | 3.7 - 5.1 s | complete |
| DirectorySize C# (as shipped) | 0.63 - 0.82 s | **incomplete**: reports "Finished" with only about 2.66 GB of 10.3 GB counted |

Numbers are from one machine and one directory, not a general benchmark. On Linux (Docker, `/usr`) the C++ CLI total matches
`find -type f -o -type l -printf %s` byte for byte.

## Legacy C# version (.NET 4.6.1)

One C# application that find the biggest directories in one selected directory

I wrote this app when I want find some app to stats size of directories in my latop. I'd found https://windirstat.net/ but it is slow and some time error. 

This app is written with C# and open Source; It's small app and run with .NET 4.6.1 or higher and (important) it  is very light!

#Directoy Size Screenshot
![Directoy Size Screenshot](https://raw.githubusercontent.com/ali33/DirectorySize/master/Directoy%20Size%20Screenshot.png)

#Directoy Size Running Screenshot
![Directoy Size Running Screenshot](https://raw.githubusercontent.com/ali33/DirectorySize/master/Directoy%20Size%20Running%20Screenshot.png)

#DirectorySizeCalculator Class
![DirectorySizeCalculator Class](https://raw.githubusercontent.com/ali33/DirectorySize/master/DirectorySizeCalculator.PNG)

DirectorySize is Open Source software. You can redistribute and/or modify it under the terms of the GNU Public License, version 2 (GPLv2). 


---
Copyright © 2026 META.vn — Author: Nguyễn Đức Sơn
