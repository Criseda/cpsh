#!/bin/bash

# Get the path to the home directory
HOME_DIR=$HOME

# Define the path to the history file
HISTORY_FILE="$HOME_DIR/.cpsh_history"

# Remove the history file if it exists
if [ -f "$HISTORY_FILE" ]; then
    rm "$HISTORY_FILE"
    echo "Removed $HISTORY_FILE"
else
    echo "$HISTORY_FILE does not exist."
fi

# Add any other files or directories you need to remove here

echo "Uninstallation complete."