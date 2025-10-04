# Chakravyuha: A Web-Based C/C++ Obfuscation Service

This repository contains the source code for a web service that demonstrates the **Chakravyuha obfuscation engine**, a tool for protecting C/C++ applications from reverse engineering. This proof-of-concept showcases a robust String Encryption pass built on the LLVM compiler framework.

The live service is hosted for free on Render and GitHub Pages.

---

## Architecture

This service uses a modern, containerized, client-server model:

*   **Frontend:** A static HTML/CSS/JS user interface hosted on **GitHub Pages**.
*   **Backend:** A Node.js server running inside a **Docker container**, hosted on **Render**.
*   **Obfuscation Core:** The C++ LLVM pass that is compiled and executed securely inside the Docker container for each request.

This monorepo contains the code for all three components.

---

## How to Deploy This Project Yourself

### Prerequisites

*   A [GitHub](https://github.com) account.
*   A [Render.com](https://render.com) account.
*   [Git](https://git-scm.com/) and [Node.js](https://nodejs.org/) installed locally for managing dependencies.

### Deployment Steps

**1. Fork and Clone the Repository:**
   Fork this repository to your own GitHub account, then clone it locally.

**2. Deploy the Backend to Render:**
   - On the Render dashboard, click **New +** -> **Web Service**.
   - Connect your forked GitHub repository.
   - On the settings page, ensure the **Environment** is set to **Docker**. Render will automatically find and use the `Dockerfile`.
   - Choose the **Free** instance type.
   - Click **Create Web Service**.
   - After the initial build (which may take several minutes), Render will provide you with a public URL (e.g., `https://your-app-name.onrender.com`).

**3. Update and Deploy the Frontend to GitHub Pages:**
   - Open the `index.html` file.
   - Find the `backendUrl` constant in the `<script>` tag at the bottom.
   - Replace the placeholder URL with the public URL of your Render backend.
   - Commit and push this change to your repository.

   - In your GitHub repository's settings, go to the **Pages** tab.
   - Under "Branch," select **`main`** and `/ (root)`, then click **Save**.
   - Your frontend will be deployed at a URL like `https://your-username.github.io/your-repo-name/`.

Your web service is now live.

---

### Local Development

1.  **Prerequisites:** You need a full Linux build environment with `clang`, `llvm-dev`, `cmake`, and `nodejs`. Using WSL on Windows is recommended.
2.  **Build:** Run `npm install`, then build the C++ pass with `mkdir build && cd build && cmake .. && cmake --build .`.
3.  **Run:** From the root directory, run `node server.js`. The backend will start on `http://localhost:3001`. You can then open `index.html` in your browser and temporarily change the `backendUrl` to point to your local server for testing.