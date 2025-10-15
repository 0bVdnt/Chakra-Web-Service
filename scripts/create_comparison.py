#!/usr/bin/env python3
import json
import os
import shutil
import subprocess
import sys
from pathlib import Path
from concurrent.futures import ProcessPoolExecutor

# --- Configuration ---
# All paths are relative to the session directory, which is the CWD.
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
    """Converts a single .dot file to an .svg file."""
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
    """Finds .dot files and renders them into SVG images in parallel."""
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
        dot_executables = [dot] * len(all_dots)
        results = list(executor.map(render_dot_to_svg, all_dots, dot_executables))

    if not all(results):
        print("Error: Some images failed to render.", file=sys.stderr)
        return False
    
    print("Python script: Image rendering complete.")
    return True

def create_comparison_html():
    """Generates the final interactive HTML file."""
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

    # =========================================================================
    # THE FINAL FIX IS HERE:
    # By using double curly braces `{{` and `}}`, we tell Python's f-string
    # engine to output literal `{` and `}` characters. This ensures the
    # JavaScript template literal `${{func}}` is written correctly into the
    # HTML file, which the browser can then execute properly.
    # =========================================================================
    html_template = f"""
    <!DOCTYPE html>
    <html lang="en">
    <head>
        <meta charset="UTF-8">
        <title>Chakravyuha - Visual Comparison Report</title>
        <style>
            body {{ font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, sans-serif; background: #f5f7fa; color: #2c3e50; padding: 20px; }}
            .container {{ max-width: 1600px; margin: 0 auto; background: white; border-radius: 16px; box-shadow: 0 20px 60px rgba(0,0,0,0.1); overflow: hidden; }}
            .header {{ background: linear-gradient(135deg, #667eea 0%, #764ba2 100%); color: white; padding: 40px; text-align: center; }}
            .header h1 {{ font-size: 2.5em; margin: 0; }}
            .controls {{ padding: 20px; background: #f8f9fa; border-bottom: 2px solid #e9ecef; display: flex; gap: 20px; align-items: center; }}
            .controls select {{ padding: 12px; border-radius: 8px; border: 2px solid #dee2e6; font-size: 16px; min-width: 250px; }}
            .comparison-container {{ padding: 40px; }}
            .image-container {{ display: flex; gap: 30px; justify-content: center; align-items: flex-start; }}
            .image-wrapper {{ flex: 1; text-align: center; max-width: 50%; }}
            .image-wrapper h3 {{ margin-bottom: 20px; font-size: 1.5em; padding: 10px; background: #e9ecef; border-radius: 8px; }}
            .image-wrapper img {{ max-width: 100%; height: auto; border: 3px solid #dee2e6; border-radius: 12px; box-shadow: 0 10px 30px rgba(0,0,0,0.1); }}
            .no-data {{ text-align: center; padding: 80px 40px; color: #6c757d; font-size: 1.4em; }}
        </style>
    </head>
    <body>
        <div class="container">
            <div class="header"><h1>Visual Comparison of Control Flow Graphs</h1></div>
            <div class="controls"><select id="functionSelect"></select></div>
            <div class="comparison-container">
                <div id="imageContainer" class="image-container"><div class="no-data">Select a function to view comparison</div></div>
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
                    funcSelect.innerHTML = '<option>No functions found</option>';
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
    print(f"Python script: Created comparison viewer at {html_file}")
    return True

def main():
    if generate_visualizations():
        create_comparison_html()

if __name__ == "__main__":
    main()