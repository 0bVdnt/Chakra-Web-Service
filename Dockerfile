# Start from a base Ubuntu 22.04 image
FROM ubuntu:22.04

# Avoid interactive prompts during installation
ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && \
    # Install prerequisites for adding new repositories
    apt-get install -y wget gnupg software-properties-common && \
    \
    # Add the official LLVM GPG key
    wget -O - https://apt.llvm.org/llvm-snapshot.gpg.key | apt-key add - && \
    \
    # Add the LLVM 20 repository for Ubuntu 22.04 (jammy)
    add-apt-repository "deb http://apt.llvm.org/jammy/ llvm-toolchain-jammy-20 main" && \
    \
    # Update package lists again to include the new repository
    apt-get update && \
    \
    # Install all system dependencies, now specifying version 20
    apt-get install -y build-essential clang-20 llvm-20-dev cmake curl git && \
    \
    # Install Node.js (same as before)
    curl -fsSL https://deb.nodesource.com/setup_lts.x | bash - && \
    apt-get install -y nodejs && \
    \
    # Clean up installation files
    apt-get clean && \
    rm -rf /var/lib/apt/lists/*

# Set environment variables to make CMake and other tools automatically use clang-20
ENV CC=/usr/bin/clang-20
ENV CXX=/usr/bin/clang++-20

# Set the working directory inside the container
WORKDIR /app

# Copy project files into the container
COPY . .

# Install Node.js dependencies
RUN npm install

# Build the C++ LLVM pass (this will now correctly use Clang/LLVM 20)
RUN mkdir -p build && \
    cd build && \
    cmake .. && \
    cmake --build .

# Expose the port the server will run on
EXPOSE 3001

# The command to run when the container starts
CMD ["node", "server.js"]