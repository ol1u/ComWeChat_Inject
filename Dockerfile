# Use a base Ubuntu image
FROM ubuntu:22.04

# Set frontend to noninteractive to avoid prompts during installation
ENV DEBIAN_FRONTEND=noninteractive

# Enable 32-bit architecture and update package lists
RUN dpkg --add-architecture i386 \
    && apt-get update \
    && apt-get install -y --no-install-recommends \
        wget \
        gpg-agent \
        software-properties-common \
        cabextract \
    # Clean up
    && rm -rf /var/lib/apt/lists/*

# Add WineHQ repository key and repository
RUN mkdir -pm755 /etc/apt/keyrings \
    && wget -O /etc/apt/keyrings/winehq-archive.key https://dl.winehq.org/wine-builds/winehq.key \
    && wget -NP /etc/apt/sources.list.d/ https://dl.winehq.org/wine-builds/ubuntu/dists/jammy/winehq-jammy.sources \
    && apt-get update

# Install WineHQ stable branch (or choose devel/staging)
# Install winetricks
RUN apt-get install -y --install-recommends winehq-stable winetricks \
    # Clean up
    && rm -rf /var/lib/apt/lists/*

# Set up Wine environment variables (use 32-bit prefix for potentially better compatibility)
ENV WINEARCH=win32
ENV WINEPREFIX=/wineprefix
ENV WINEDEBUG=-all # Disable Wine debug messages by default

# Create Wine prefix directory and set permissions
RUN mkdir $WINEPREFIX \
    && chown nobody:nogroup $WINEPREFIX

# Switch to a non-root user
USER nobody

# Initialize Wine prefix and install common dependencies (e.g., corefonts, vcrun - adjust as needed)
# Running wineboot updates the prefix
# Winetricks commands might fail if run non-interactively without specific overrides,
# but let's try installing some common items.
RUN wine wineboot --init \
    && winetricks -q corefonts vcrun2019 # Add other necessary components here

# Create a directory for the applications
WORKDIR /app

# Copy the injector executable into the image
# IMPORTANT: Place WeChatHook.exe in the same directory as the Dockerfile before building
COPY --chown=nobody:nogroup WeChatHook.exe /app/WeChatHook.exe

# Copy the target application (e.g., WeChat installer or portable version)
# IMPORTANT: Place WeChat files/installer in the same directory as the Dockerfile
# Example: COPY --chown=nobody:nogroup WeChatSetup.exe /app/WeChatSetup.exe
# Example: COPY --chown=nobody:nogroup WeChat/ /app/WeChat/

# Set the working directory back to user's home in wineprefix
WORKDIR $WINEPREFIX/drive_c/users/nobody

# Default command (placeholder - needs customization)
# This is highly dependent on how WeChat is installed and how the injector should be run.
# You might need a custom entrypoint script.
# Example: Run WeChat first, then the injector (this is a simplified example)
# CMD ["wine", "/app/WeChat/WeChat.exe"] # Adjust path to WeChat executable
# Followed by running the injector in another command or via a script.
# A script is generally better for managing multiple processes.
CMD ["wine", "explorer"]
