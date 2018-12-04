namespace DirectorySize
{
    partial class Form1
    {
        /// <summary>
        /// Required designer variable.
        /// </summary>
        private System.ComponentModel.IContainer components = null;

        /// <summary>
        /// Clean up any resources being used.
        /// </summary>
        /// <param name="disposing">true if managed resources should be disposed; otherwise, false.</param>
        protected override void Dispose(bool disposing)
        {
            if (disposing && (components != null))
            {
                components.Dispose();
            }
            base.Dispose(disposing);
        }

        #region Windows Form Designer generated code

        /// <summary>
        /// Required method for Designer support - do not modify
        /// the contents of this method with the code editor.
        /// </summary>
        private void InitializeComponent()
        {
            this.txtFolderPath = new System.Windows.Forms.TextBox();
            this.dlgFolderBrowser = new System.Windows.Forms.FolderBrowserDialog();
            this.btnChoseFolder = new System.Windows.Forms.Button();
            this.btnAnalyzer = new System.Windows.Forms.Button();
            this.pnlResults = new System.Windows.Forms.Panel();
            this.textBox1 = new System.Windows.Forms.TextBox();
            this.lblStats = new System.Windows.Forms.Label();
            this.label1 = new System.Windows.Forms.Label();
            this.btnStop = new System.Windows.Forms.Button();
            this.pnlResults.SuspendLayout();
            this.SuspendLayout();
            // 
            // txtFolderPath
            // 
            this.txtFolderPath.Location = new System.Drawing.Point(12, 12);
            this.txtFolderPath.Name = "txtFolderPath";
            this.txtFolderPath.Size = new System.Drawing.Size(468, 20);
            this.txtFolderPath.TabIndex = 0;
            this.txtFolderPath.Text = "C:\\";
            // 
            // btnChoseFolder
            // 
            this.btnChoseFolder.Location = new System.Drawing.Point(486, 11);
            this.btnChoseFolder.Name = "btnChoseFolder";
            this.btnChoseFolder.Size = new System.Drawing.Size(91, 23);
            this.btnChoseFolder.TabIndex = 1;
            this.btnChoseFolder.Text = "Select Folder";
            this.btnChoseFolder.UseVisualStyleBackColor = true;
            this.btnChoseFolder.Click += new System.EventHandler(this.btnChoseFolder_Click);
            // 
            // btnAnalyzer
            // 
            this.btnAnalyzer.Location = new System.Drawing.Point(583, 11);
            this.btnAnalyzer.Name = "btnAnalyzer";
            this.btnAnalyzer.Size = new System.Drawing.Size(75, 23);
            this.btnAnalyzer.TabIndex = 2;
            this.btnAnalyzer.Text = "Calculate";
            this.btnAnalyzer.UseVisualStyleBackColor = true;
            this.btnAnalyzer.Click += new System.EventHandler(this.btnAnalyzer_Click);
            // 
            // pnlResults
            // 
            this.pnlResults.Controls.Add(this.textBox1);
            this.pnlResults.Location = new System.Drawing.Point(12, 57);
            this.pnlResults.Name = "pnlResults";
            this.pnlResults.Size = new System.Drawing.Size(707, 325);
            this.pnlResults.TabIndex = 3;
            // 
            // textBox1
            // 
            this.textBox1.Location = new System.Drawing.Point(3, 3);
            this.textBox1.Multiline = true;
            this.textBox1.Name = "textBox1";
            this.textBox1.ScrollBars = System.Windows.Forms.ScrollBars.Vertical;
            this.textBox1.Size = new System.Drawing.Size(701, 319);
            this.textBox1.TabIndex = 0;
            // 
            // lblStats
            // 
            this.lblStats.Location = new System.Drawing.Point(529, 396);
            this.lblStats.Name = "lblStats";
            this.lblStats.Size = new System.Drawing.Size(187, 13);
            this.lblStats.TabIndex = 4;
            this.lblStats.Text = "*";
            this.lblStats.TextAlign = System.Drawing.ContentAlignment.TopRight;
            // 
            // label1
            // 
            this.label1.AutoSize = true;
            this.label1.Location = new System.Drawing.Point(12, 396);
            this.label1.Name = "label1";
            this.label1.Size = new System.Drawing.Size(11, 13);
            this.label1.TabIndex = 5;
            this.label1.Text = "*";
            // 
            // btnStop
            // 
            this.btnStop.Enabled = false;
            this.btnStop.Location = new System.Drawing.Point(665, 12);
            this.btnStop.Name = "btnStop";
            this.btnStop.Size = new System.Drawing.Size(51, 23);
            this.btnStop.TabIndex = 6;
            this.btnStop.Text = "Stop!";
            this.btnStop.UseVisualStyleBackColor = true;
            this.btnStop.Click += new System.EventHandler(this.btnStop_Click);
            // 
            // Form1
            // 
            this.AutoScaleDimensions = new System.Drawing.SizeF(6F, 13F);
            this.AutoScaleMode = System.Windows.Forms.AutoScaleMode.Font;
            this.ClientSize = new System.Drawing.Size(734, 450);
            this.Controls.Add(this.btnStop);
            this.Controls.Add(this.label1);
            this.Controls.Add(this.lblStats);
            this.Controls.Add(this.pnlResults);
            this.Controls.Add(this.btnAnalyzer);
            this.Controls.Add(this.btnChoseFolder);
            this.Controls.Add(this.txtFolderPath);
            this.MaximizeBox = false;
            this.Name = "Form1";
            this.Text = "Top Directories";
            this.pnlResults.ResumeLayout(false);
            this.pnlResults.PerformLayout();
            this.ResumeLayout(false);
            this.PerformLayout();

        }

        #endregion

        private System.Windows.Forms.TextBox txtFolderPath;
        private System.Windows.Forms.FolderBrowserDialog dlgFolderBrowser;
        private System.Windows.Forms.Button btnChoseFolder;
        private System.Windows.Forms.Button btnAnalyzer;
        private System.Windows.Forms.Panel pnlResults;
        private System.Windows.Forms.TextBox textBox1;
        private System.Windows.Forms.Label lblStats;
        private System.Windows.Forms.Label label1;
        private System.Windows.Forms.Button btnStop;
    }
}

