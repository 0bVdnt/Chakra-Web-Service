# Start from a base Ubuntu 22.04 image
FROM ubuntu:22.04

# Avoid interactive prompts during installation
ENV DEBIAN_FRONTEND=noninteractive

# Install LLVM/Clang version 20
RUN apt-get update && \
    apt-get install -y wget gnupg software-properties-common && \
    wget -O - https://apt.llvm.org/llvm-snapshot.gpg.key | apt-key add - && \
    add-apt-repository "deb http://apt.llvm.org/jammy/ llvm-toolchain-jammy-20 main" && \
    apt-get update && \
    apt-get install -y build-essential clang-20 llvm-20-dev cmake curl git && \
    curl -fsSL https://deb.nodesource.com/setup_lts.x | bash - && \
    apt-get install -y nodejs && \
    apt-get clean && \
    rm -rf /var/lib/apt/lists/*

RUN ln -s /usr/bin/clang-20 /usr/bin/clang && \
    ln -s /usr/bin/clang++-20 /usr/bin/clang++ && \
    ln -s /usr/bin/opt-20 /usr/bin/opt

ENV CC=/usr/bin/clang-20
ENV CXX=/usr/bin/clang++-20

# Set the working directory inside the container
WORKDIR /app

# Copy project files into the container
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