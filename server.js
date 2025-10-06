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

app.get('/', (req, res) => {
    res.status(200).json({ 
        status: 'ok', 
        message: 'Chakravyuha backend is running.' 
    });
});

app.post('/obfuscate', (req, res) => {
    const { code, options } = req.body;
    if (!code) {
        return res.status(400).json({ error: 'Missing code parameter.' });
    }

    const sessionId = crypto.randomBytes(16).toString('hex');
    const sessionDir = path.join(TEMP_DIR, sessionId);
    fs.mkdirSync(sessionDir, { recursive: true });

    const sourceFilePath = path.join(sessionDir, 'source_code.c');
    fs.writeFileSync(sourceFilePath, code);

    const scriptPath = path.join(__dirname, 'scripts', 'run_obfuscation.sh');
    const buildDir = path.join(sessionDir, 'build');
    fs.mkdirSync(buildDir);

    // Determine which pass pipeline to run based on frontend options
    const enableCFF = options && options.enableCFF;
    const passesArg = enableCFF ? "chakravyuha-full" : "chakravyuha-str-only";

    const command = `bash "${scriptPath}" "${sourceFilePath}" "${buildDir}" "${passesArg}"`;
    console.log(`[Job ${sessionId}] Executing: ${command}`);
    
    exec(command, { cwd: __dirname }, (error, stdout, stderr) => {
        if (error) {
            console.error(`[Job ${sessionId}] Script failed:`, error);
            const fullOutput = stdout + stderr;
            rimraf(sessionDir, () => {});
            return res.status(500).json({ error: 'Obfuscation script failed.', details: `--- SCRIPT OUTPUT ---\n${fullOutput}`});
        }
        
        const reportPath = path.join(buildDir, 'report.json');
        const cffMetricsPath = path.join(buildDir, 'cff_metrics.json');

        if (!fs.existsSync(reportPath)) {
            const fullOutput = stdout + stderr;
            rimraf(sessionDir, () => {});
            return res.status(500).json({ error: 'Script ran, but the final report was not found.', details: `--- SCRIPT OUTPUT ---\n${fullOutput}`});
        }

        try {
            const reportJson = JSON.parse(fs.readFileSync(reportPath, 'utf8'));
            let cffMetricsJson = null;

            if (fs.existsSync(cffMetricsPath)) {
                 const cffMetricsRaw = fs.readFileSync(cffMetricsPath, 'utf8');
                 if(cffMetricsRaw.trim().length > 0) {
                    cffMetricsJson = JSON.parse(cffMetricsRaw);
                 }
            }
            
            const downloadPath = `/download/${sessionId}/obfuscated_program`;
            res.status(200).json({ 
                success: true, 
                report: reportJson, 
                cffMetrics: cffMetricsJson,
                downloadPath: downloadPath 
            });

        } catch (e) {
            rimraf(sessionDir, () => {});
            return res.status(500).json({ error: 'Failed to parse output reports.', details: e.message });
        }
    });
});

app.get('/download/:sessionId/:binaryName', (req, res) => {
    const { sessionId, binaryName } = req.params;
    const sessionDir = path.join(TEMP_DIR, sessionId);
    const binaryPath = path.join(sessionDir, 'build', binaryName);
    
    if (!fs.existsSync(binaryPath)) {
        return res.status(404).send('File not found or session has expired.');
    }

    res.download(binaryPath, binaryName, (err) => {
        if (err) {
            console.error(`[Job ${sessionId}] Download Error: ${err.message}`);
        }
        // Clean up the session directory after the download is complete or fails
        rimraf(sessionDir, () => {});
    });
});

app.listen(PORT, '0.0.0.0', () => {
    console.log(`Chakravyuha backend server running on port ${PORT}`);
});
