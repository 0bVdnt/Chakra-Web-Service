const express = require('express');
const { exec } = require('child_process');
const fs = require('fs');
const path = require('path');
const crypto = require('crypto');
const cors = require('cors');
const rimraf = require('rimraf');

const app = express();
const PORT = process.env.PORT || 3001;
const TEMP_DIR = path.join(__dirname, 'temp_sessions');

if (!fs.existsSync(TEMP_DIR)) fs.mkdirSync(TEMP_DIR);
app.use(cors());
app.use(express.json({ limit: '5mb' }));

app.post('/obfuscate', (req, res) => {
    const { code } = req.body;
    if (!code) return res.status(400).json({ error: 'Missing code parameter.' });

    const sessionId = crypto.randomBytes(16).toString('hex');
    const sessionDir = path.join(TEMP_DIR, sessionId);
    fs.mkdirSync(sessionDir, { recursive: true });

    const sourceFilePath = path.join(sessionDir, 'test_program.c');
    fs.writeFileSync(sourceFilePath, code);

    const scriptPath = path.join(__dirname, 'scripts', 'run_obfuscation.sh');
    const buildDir = path.join(sessionDir, 'build');
    fs.mkdirSync(buildDir);

    const command = `bash "${scriptPath}" "${sourceFilePath}" "${buildDir}"`;
    console.log(`[Job ${sessionId}] Executing: ${command}`);
    
    exec(command, { cwd: __dirname }, (error, stdout, stderr) => {
        const fullOutput = stdout + stderr;
        if (error) {
            console.error(`[Job ${sessionId}] Script failed:`, error);
            rimraf(sessionDir, () => {});
            return res.status(500).json({ error: 'Linux obfuscation script failed.', details: `--- SCRIPT OUTPUT ---\n${fullOutput}`});
        }
        
        const reportPath = path.join(buildDir, 'report.json');
        if (!fs.existsSync(reportPath)) {
            rimraf(sessionDir, () => {});
            return res.status(500).json({ error: 'Script ran, but report was not found.', details: `--- SCRIPT OUTPUT ---\n${fullOutput}`});
        }

        try {
            const reportJson = JSON.parse(fs.readFileSync(reportPath, 'utf8'));
            const downloadPath = `/download/${sessionId}/obfuscated_program`;
            res.status(200).json({ success: true, report: reportJson, downloadPath: downloadPath });
        } catch (e) {
            rimraf(sessionDir, () => {});
            return res.status(500).json({ error: 'Failed to parse report.', details: fs.readFileSync(reportPath, 'utf8')});
        }
    });
});

app.get('/download/:sessionId/:binaryName', (req, res) => {
    const { sessionId, binaryName } = req.params;
    const sessionDir = path.join(TEMP_DIR, sessionId);
    const binaryPath = path.join(sessionDir, 'build', binaryName);
    if (!fs.existsSync(binaryPath)) return res.status(404).send('File not found or session expired.');
    res.download(binaryPath, binaryName, (err) => {
        if (err) console.error(`[Job ${sessionId}] Download Error: ${err.message}`);
        rimraf(sessionDir, () => {});
    });
});

app.listen(PORT, '0.0.0.0', () => {
    console.log(`Chakravyuha backend server running on port ${PORT}`);
});