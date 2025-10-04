FROM ubuntu:22.04

# Avoid interactive prompts during installation
ENV DEBIAN_FRONTEND=noninteractive

# Install all system dependencies: LLVM/Clang, CMake, and Node.js
RUN apt-get update && \
    apt-get install -y build-essential clang llvm-dev cmake curl git && \
    curl -fsSL https://deb.nodesource.com/setup_lts.x | bash - && \
    apt-get install -y nodejs && \
    apt-get clean && \
    rm -rf /var/lib/apt/lists/*

# Set the working directory inside the container
WORKDIR /app

# Copy your project files into the container.
# .dockerignore will be used to exclude node_modules, etc.
COPY . .

# Install Node.js dependencies
RUN npm install

# Build the C++ LLVM pass
RUN mkdir -p build && \
    cd build && \
    cmake .. && \
    cmake --build .

# Expose the port the server will run on
EXPOSE 3001

# The command to run when the container starts
CMD ["node", "server.js"]