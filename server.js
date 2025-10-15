const express = require('express');
const { execFile } = require('child_process');
const fs = require('fs');
const path = require('path');
const crypto = require('crypto');
const cors = require('cors');
const rimraf = require('rimraf');

const app = express();
const PORT = process.env.PORT || 3001;
const TEMP_DIR = path.join(__dirname, 'temp_sessions');

// Create a temporary directory for session files on startup
if (fs.existsSync(TEMP_DIR)) rimraf.sync(TEMP_DIR);
fs.mkdirSync(TEMP_DIR);

// --- Middleware Setup ---
app.use(cors());
app.use(express.json({ limit: '5mb' }));

// --- Static File Serving ---
// This line serves your logo from the '/assets' folder
app.use('/assets', express.static(path.join(__dirname, 'assets')));
// This line serves the dynamically generated comparison pages
app.use('/comparison', express.static(TEMP_DIR));


// --- Routes ---
app.get('/', (req, res) => res.status(200).json({ status: 'ok', message: 'Chakravyuha backend is running.' }));

// Helper function to format file sizes for the report
function formatBytes(bytes, decimals = 2) {
    if (bytes === 0) return '0 Bytes';
    const k = 1024;
    const dm = decimals < 0 ? 0 : decimals;
    const sizes = ['Bytes', 'KB', 'MB', 'GB', 'TB'];
    const i = Math.floor(Math.log(bytes) / Math.log(k));
    return parseFloat((bytes / Math.pow(k, i)).toFixed(dm)) + ' ' + sizes[i];
}

// Main obfuscation endpoint
app.post('/obfuscate', (req, res) => {
    const { code, pipeline, cycles = 1, level = 'medium', generateCfg = false, filename = 'source.cpp' } = req.body;
    if (!code) {
        return res.status(400).json({ error: 'Missing code parameter.' });
    }
    
    const cycleCount = parseInt(cycles, 10);
    if (isNaN(cycleCount) || cycleCount < 1 || cycleCount > 5) {
        return res.status(400).json({ error: 'Cycles must be between 1 and 5.' });
    }

    // Create a unique session directory for this request
    const sessionId = crypto.randomBytes(16).toString('hex');
    const sessionDir = path.join(TEMP_DIR, sessionId);
    fs.mkdirSync(sessionDir, { recursive: true });

    const sourceFilePath = path.join(sessionDir, filename);
    fs.writeFileSync(sourceFilePath, code);

    const buildDir = path.join(sessionDir, 'build');
    fs.mkdirSync(buildDir);

    // Prepare and execute the main obfuscation shell script
    const scriptPath = path.join(__dirname, 'scripts', 'run_obfuscation.sh');
    const args = [scriptPath, sourceFilePath, buildDir, pipeline, cycleCount.toString(), generateCfg.toString(), filename];

    console.log(`[Job ${sessionId}] Executing: bash ${args.join(' ')}`);
    execFile('bash', args, { cwd: __dirname }, (error, stdout, stderr) => {
        const fullOutput = stdout + stderr;
        if (error) {
            console.error(`[Job ${sessionId}] Obfuscation script failed:`, error, `Output:\n${fullOutput}`);
            rimraf.sync(sessionDir);
            return res.status(500).json({ error: 'Obfuscation script failed.', details: `--- SCRIPT OUTPUT ---\n${fullOutput}` });
        }
        console.log(`[Job ${sessionId}] Obfuscation script finished.`);

        // Function to parse report files and send the final response
        const processAndRespond = () => {
            const reportPath = path.join(buildDir, 'report.json');
            const sizeMetricsPath = path.join(buildDir, 'size_metrics.json');

            if (!fs.existsSync(reportPath)) {
                rimraf.sync(sessionDir);
                return res.status(500).json({ error: 'Report.json was not generated.', details: `--- SCRIPT OUTPUT ---\n${fullOutput}` });
            }
            try {
                const reportJson = JSON.parse(fs.readFileSync(reportPath, 'utf8'));
                reportJson.inputParameters.obfuscationLevel = level;
                reportJson.obfuscationMetrics.cyclesCompleted = cycleCount;

                // If binary size metrics exist, add them to the report
                if (fs.existsSync(sizeMetricsPath)) {
                    const sizeMetrics = JSON.parse(fs.readFileSync(sizeMetricsPath, 'utf8'));
                    const origSize = sizeMetrics.originalSize;
                    const obfSize = sizeMetrics.obfuscatedSize;
                    let changePct = 0;
                    if (origSize > 0) {
                        changePct = ((obfSize - origSize) / origSize) * 100;
                    }
                    reportJson.binaryMetrics = {
                        originalSize: formatBytes(origSize),
                        obfuscatedSize: formatBytes(obfSize),
                        sizeChange: `${changePct.toFixed(2)}%`
                    };
                }

                const responsePayload = {
                    success: true,
                    report: reportJson,
                    downloadPath: `/download/${sessionId}/obfuscated_program`
                };

                // If CFG was generated, add the path to the comparison page
                if (generateCfg) {
                    const comparisonPage = path.join('build', 'visualizations', 'comparison', 'index.html');
                    responsePayload.comparisonPagePath = `/comparison/${sessionId}/${comparisonPage}`;
                }

                res.status(200).json(responsePayload);
            } catch (e) {
                rimraf.sync(sessionDir);
                return res.status(500).json({ error: 'Failed to process report files.', details: e.message });
            }
        };

        // If CFG generation was requested, run the Python visualization script first
        if (generateCfg) {
            console.log(`[Job ${sessionId}] Generating CFG visualizations...`);
            const vizScriptPath = path.join(__dirname, 'scripts', 'create_comparison.py');
            execFile('python3', [vizScriptPath], { cwd: sessionDir }, (vizError, vizStdout, vizStderr) => {
                if (vizError) {
                    console.error(`[Job ${sessionId}] Visualization script failed:`, vizError, `Output:\n${vizStdout}\n${vizStderr}`);
                    rimraf.sync(sessionDir);
                    return res.status(500).json({ error: 'Visualization script failed.', details: `--- SCRIPT OUTPUT ---\n${vizStdout}\n${vizStderr}` });
                }
                console.log(`[Job ${sessionId}] Visualization finished.`);
                processAndRespond();
            });
        } else {
            processAndRespond();
        }
    });
});

// Download endpoint for the obfuscated binary
app.get('/download/:sessionId/:binaryName', (req, res) => {
    const { sessionId, binaryName } = req.params;
    const filePath = path.join(TEMP_DIR, sessionId, 'build', binaryName);

    if (fs.existsSync(filePath)) {
        res.download(filePath, (err) => {
            if (err) {
                console.error(`[Download Error] For session ${sessionId}:`, err);
            }
            // Clean up the session directory after a successful download
            rimraf(path.join(TEMP_DIR, sessionId), (rimrafErr) => {
                if (rimrafErr) console.error(`Failed to clean up session ${sessionId}:`, rimrafErr);
            });
        });
    } else {
        res.status(404).json({ error: 'File not found. It may have been downloaded already or expired.' });
    }
});

// --- Start Server ---
app.listen(PORT, '0.0.0.0', () => {
    console.log(`Chakravyuha backend server running on port ${PORT}`);
});