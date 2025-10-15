import json
import os
import shutil
import subprocess
import sys
from pathlib import Path
from concurrent.futures import ProcessPoolExecutor

# --- Configuration ---
session_dir = Path.cwd()
build_dir = session_dir / "build"
results_dir = build_dir

# --- Helper functions ---
def find_executable(name, msg):
    path = shutil.which(name)
    if not path:
        print(f"Error: {msg}", file=sys.stderr)
        sys.exit(1)
    return path

# --- Parallel Rendering Task ---
def render_dot_to_svg(dot_file_path, dot_executable):
    is_original = dot_file_path.parent.name == "original"
    target_dir = results_dir / "visualizations" / ("original" if is_original else "obfuscated")
    svg_file = target_dir / f"{dot_file_path.stem.strip('.')}.svg"
    try:
        subprocess.run(
            [dot_executable, "-Tsvg", str(dot_file_path), "-o", str(svg_file)],
            check=True, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True
        )
    except subprocess.CalledProcessError as e:
        print(f"Failed to render {dot_file_path}: {e.stderr}", file=sys.stderr)
        return False
    return True

# --- Main Logic ---
def generate_visualizations():
    print("Python script: Rendering CFG images...")
    dot = find_executable("dot", "'dot' (from graphviz) not found.")
    
    dot_base_dir = results_dir / "dot_files"
    if not dot_base_dir.exists():
        print(f"Error: DOT file directory '{dot_base_dir}' not found.", file=sys.stderr)
        return False

    orig_dot_dir = dot_base_dir / "original"
    obf_dot_dir = dot_base_dir / "obfuscated"
    
    (results_dir / "visualizations" / "original").mkdir(parents=True, exist_ok=True)
    (results_dir / "visualizations" / "obfuscated").mkdir(parents=True, exist_ok=True)

    all_dots = list(orig_dot_dir.glob("*.dot")) + list(obf_dot_dir.glob("*.dot"))
    if not all_dots:
        print("Warning: No .dot files were found to visualize.", file=sys.stderr)
        return True

    with ProcessPoolExecutor() as executor:
        results = list(executor.map(render_dot_to_svg, all_dots, [dot] * len(all_dots)))

    if not all(results):
        print("Error: Some images failed to render.", file=sys.stderr)
        return False
    
    print("Python script: Image rendering complete.")
    return True

def create_comparison_html():
    print("Python script: Generating interactive HTML report...")
    comparison_dir = results_dir / "visualizations" / "comparison"
    comparison_dir.mkdir(parents=True, exist_ok=True)

    tests = {}
    test_name = "test_program"
    tests[test_name] = {}
    
    orig_img_dir = results_dir / "visualizations" / "original"
    obf_img_dir = results_dir / "visualizations" / "obfuscated"
    
    if orig_img_dir.exists():
        for orig_img in orig_img_dir.glob("*.svg"):
            func_name = orig_img.stem.strip('.')
            obf_img = obf_img_dir / orig_img.name
            if obf_img.exists():
                tests[test_name][func_name] = {
                    "original": str(orig_img.relative_to(results_dir)),
                    "obfuscated": str(obf_img.relative_to(results_dir)),
                }

    html_template = f"""
    <!DOCTYPE html>
    <html lang="en">
    <head>
        <meta charset="UTF-8">
        <title>Chakravyuha - Visual Comparison Report</title>
        <style>
            :root {{
                --bg-color: #f8f9fa;
                --container-bg: #ffffff;
                --header-bg: #2c3e50;
                --text-color: #343a40;
                --border-color: #dee2e6;
            }}
            body {{
                font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, sans-serif;
                background: var(--bg-color);
                color: var(--text-color);
                margin: 0;
                padding: 2rem;
            }}
            .container {{
                max-width: 1600px;
                margin: 0 auto;
                background: var(--container-bg);
                border-radius: 8px;
                box-shadow: 0 6px 20px rgba(0,0,0,0.08);
                overflow: hidden;
            }}
            .header {{
                background: var(--header-bg);
                color: white;
                padding: 1.5rem 2rem;
                text-align: center;
            }}
            .header .logo {{
                width: 120px;
                height: 120px;
                margin: 0 auto 1rem;
                display: block;
                border-radius: 8px;
            }}
            .header h2 {{
                font-size: 2em;
                margin: 0;
                font-weight: 600;
            }}
            .controls {{
                padding: 20px;
                background: #fdfdff;
                border-bottom: 1px solid var(--border-color);
                display: flex;
                gap: 20px;
                align-items: center;
                justify-content: center;
            }}
            .controls label {{
                font-weight: 600;
                font-size: 1.1rem;
            }}
            .controls select {{
                padding: 10px;
                border-radius: 6px;
                border: 1px solid var(--border-color);
                font-size: 1rem;
                min-width: 300px;
            }}
            .comparison-container {{ padding: 2.5rem; }}
            .image-container {{
                display: flex;
                gap: 2rem;
                justify-content: center;
                align-items: flex-start;
            }}
            .image-wrapper {{
                flex: 1;
                text-align: center;
                max-width: 50%;
                background: #fdfdff;
                border: 1px solid var(--border-color);
                border-radius: 8px;
                padding: 1.5rem;
                box-shadow: 0 4px 12px rgba(0,0,0,0.05);
            }}
            .image-wrapper h3 {{
                margin-top: 0;
                margin-bottom: 1.5rem;
                font-size: 1.5em;
                padding-bottom: 0.75rem;
                border-bottom: 1px solid var(--border-color);
            }}
            .image-wrapper img {{
                max-width: 100%;
                height: auto;
                border-radius: 4px;
            }}
            .no-data {{
                text-align: center;
                padding: 80px 40px;
                color: #6c757d;
                font-size: 1.4em;
            }}
        </style>
    </head>
    <body>
        <div class="container">
            <div class="header">
                <img src="/assets/chakravyuha_logo.png" alt="Chakravyuha Logo" class="logo">
                <h2>Visual Comparison of Control Flow Graphs</h2>
            </div>
            <div class="controls">
                <label for="functionSelect">Select Function:</label>
                <select id="functionSelect"></select>
            </div>
            <div class="comparison-container">
                <div id="imageContainer" class="image-container">
                    <div class="no-data">Select a function to view comparison.</div>
                </div>
            </div>
        </div>
        <script>
            const testsData = {json.dumps(tests, indent=4)};
            const testName = "{test_name}";
            const funcSelect = document.getElementById('functionSelect');
            const imageContainer = document.getElementById('imageContainer');

            function updateFunctionList() {{
                const functions = testsData[testName] ? Object.keys(testsData[testName]).sort() : [];
                if (functions.length > 0) {{
                    funcSelect.innerHTML = functions.map(func => `<option value="${{func}}">${{func}}</option>`).join('');
                }} else {{
                    funcSelect.innerHTML = '<option>No functions found to compare</option>';
                }}
                updateDisplay();
            }}

            function updateDisplay() {{
                const funcName = funcSelect.value;
                if (!funcName || !testsData[testName]?.[funcName]) {{
                    imageContainer.innerHTML = '<div class="no-data">No data available for this function.</div>';
                }} else {{
                    const images = testsData[testName][funcName];
                    imageContainer.innerHTML = `
                        <div class="image-wrapper">
                            <h3>Original</h3>
                            <img src="../../${{images.original}}" alt="Original CFG">
                        </div>
                        <div class="image-wrapper">
                            <h3>Obfuscated</h3>
                            <img src="../../${{images.obfuscated}}" alt="Obfuscated CFG">
                        </div>
                    `;
                }}
            }}
            funcSelect.addEventListener('change', updateDisplay);
            updateFunctionList();
        </script>
    </body>
    </html>
    """
    html_file = comparison_dir / "index.html"
    with open(html_file, "w", encoding="utf-8") as f:
        f.write(html_template)
    print(f"Python script: Created themed comparison viewer at {html_file}")
    return True

def main():
    if generate_visualizations():
        create_comparison_html()

if __name__ == "__main__":
    main()