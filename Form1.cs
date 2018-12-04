using System;
using System.Collections.Generic;
using System.Collections.Concurrent;
using System.Collections.Specialized;
using System.ComponentModel;
using System.Data;
using System.Drawing;
using System.Linq;
using System.Text;
using System.Threading.Tasks;
using System.Windows.Forms;
using System.IO;
using System.Threading;
namespace DirectorySize
{
    public partial class Form1 : Form
    {
        DirectorySizeCalculator calc = null;
        public Form1()
        {
            InitializeComponent();
        }

        private void btnChoseFolder_Click(object sender, EventArgs e)
        {
            var result = dlgFolderBrowser.ShowDialog();
            if (result == DialogResult.OK || result == DialogResult.Yes)
            {
                txtFolderPath.Text = dlgFolderBrowser.SelectedPath;

            }
        }

        private void btnAnalyzer_Click(object sender, EventArgs e)
        {
            calc = new DirectorySizeCalculator(txtFolderPath.Text.Trim(), onSizeUpdated);
            calc.Calculate();
        }

        private void onSizeUpdated(SortedDictionary<string, long> sizes, bool isOk)
        {
            if (!isOk)
            {
                this.Invoke(new MethodInvoker(delegate ()
                {
                    btnAnalyzer.Enabled = false;
                    btnChoseFolder.Enabled = false;
                    btnStop.Enabled = true;
                    label1.ForeColor = Color.DarkBlue;
                    label1.Text = "running...";
                    List<string> lines = new List<string>();
                    try
                    {
                        foreach (var sz in sizes.OrderByDescending(s => s.Value).Take(100))
                        {
                            lines.Add(string.Format("{0}: {1}", sz.Key, GetFileSize(sz.Value)));
                        }
                    }
                    catch (Exception ex)
                    {
                        lines.Clear();
                    }
                    if (lines.Count > 0)
                    {
                        lblStats.Text = sizes.Count.ToString("#,#") + " directories";
                        textBox1.Clear();
                        textBox1.Lines = lines.ToArray();
                    }
                }));
            }
            else
            {
                this.Invoke(new MethodInvoker(delegate ()
                {
                    btnAnalyzer.Enabled = true;
                    btnChoseFolder.Enabled = true;
                    btnStop.Enabled = false;
                    label1.ForeColor = Color.Red;
                    label1.Text = "Finished!";
                }
                ));
            }
        }

        private string GetFileSize(long byteCount)
        {
            string size = "0 Bytes";
            if (byteCount >= 1073741824.0)
                size = String.Format("{0:##.##}", byteCount / 1073741824.0) + " GB";
            else if (byteCount >= 1048576.0)
                size = String.Format("{0:##.##}", byteCount / 1048576.0) + " MB";
            else if (byteCount >= 1024.0)
                size = String.Format("{0:##.##}", byteCount / 1024.0) + " KB";
            else if (byteCount > 0 && byteCount < 1024.0)
                size = byteCount.ToString() + " Bytes";

            return size;
        }

        private void btnStop_Click(object sender, EventArgs e)
        {
            calc.Stop();
            btnAnalyzer.Enabled = true;
            btnChoseFolder.Enabled = true;
            btnStop.Enabled = false;
        }
    }


    public class DirectorySizeCalculator
    {
        string _rootPath = "";
        DirectoryInfo _rootFolder;
        SortedDictionary<string, long> _sizes;
        object _locker = new object();
        Action<SortedDictionary<string, long>, bool> _onSizeUpdated;
        bool _isOk = false;
        public DirectorySizeCalculator(string rootPath, Action<SortedDictionary<string, long>, bool> onSizeUpdated = null)
        {
            _rootPath = rootPath;
            _rootFolder = new DirectoryInfo(_rootPath);
            _sizes = new SortedDictionary<string, long>();
            _onSizeUpdated = onSizeUpdated;
        }

        List<Thread> _tasks = new List<Thread>();
        public void Stop()
        {
            _tasks.ForEach(th => th.Abort());
            _tasks.Clear();
            _sizes.Clear();
            _isOk = false;
        }
        public void Calculate()
        {
            _tasks.ForEach(th => th.Abort());
            _tasks.Clear();
            _sizes.Clear();
            _isOk = false;
            if (_onSizeUpdated != null)
            {
                var thrSizeUpdated = new Thread(() =>
                  {
                      Thread.Sleep(500);
                      while (!_isOk)
                      {
                          _onSizeUpdated.Invoke(_sizes, _isOk);
                          Thread.Sleep(500);
                      }
                      _onSizeUpdated.Invoke(_sizes, _isOk);
                  });
                thrSizeUpdated.Start();
                _tasks.Add(thrSizeUpdated);
            }
            if (_rootFolder.Exists)
            {
                ConcurrentQueue<FileInfo> files = new ConcurrentQueue<FileInfo>();
                var thrCrawlFiles = new Thread(() =>
                {
                    Thread.Sleep(100);
                    CrawlFiles(files, _rootFolder, "*.*");
                    _isOk = true;
                });
                thrCrawlFiles.Start();
                _tasks.Add(thrCrawlFiles);
                var thrAddFileSize = new Thread(() =>
                {
                    while (!_isOk)
                    {
                        FileInfo file;
                        if (files == null || files.Count == 0)
                        {
                            Thread.Sleep(100);
                        }
                        else
                        {
                            if (files.TryDequeue(out file))
                            {
                                addFileSize(file, null);
                            }
                        }
                    }
                });
                thrAddFileSize.Start();
                _tasks.Add(thrAddFileSize);
            }
        }

        IEnumerable<FileInfo> EnumerateFiles(DirectoryInfo path, string searchPattern)
        {
            return path.EnumerateFiles(searchPattern).Union(path.EnumerateDirectories().SelectMany(
                d => { try { return EnumerateFiles(d, searchPattern); } catch { return new List<FileInfo>(); } }));
        }

        void CrawlFiles(ConcurrentQueue<FileInfo> files, DirectoryInfo path, string searchPattern)
        {
            foreach (var f in path.EnumerateFiles(searchPattern))
            {
                files.Enqueue(f);
            }
            foreach (var d in path.EnumerateDirectories())
            {
                try { CrawlFiles(files, d, searchPattern); }
                catch
                {

                }
            }
        }

        private void addFileSize(FileInfo file, DirectoryInfo directory)
        {
            var folder = directory == null ? file.Directory : directory;
            if (folder != null)
            {
                if (!_sizes.ContainsKey(folder.FullName))
                {
                    lock (_locker)
                    {
                        _sizes.Add(folder.FullName, file.Length);
                    }
                }
                else
                {
                    lock (_locker)
                    {
                        long size = _sizes[folder.FullName];
                        _sizes[folder.FullName] = size + file.Length;
                    }
                }
                if (!folder.FullName.Equals(_rootFolder.FullName))
                {
                    addFileSize(file, folder.Parent);
                }
            }
        }
    }
}
