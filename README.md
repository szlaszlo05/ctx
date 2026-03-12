# ctx
This windows tool recursively scans directories, filters by extension, ignores specified folders, and produces chunked text files suitable for LLM context windows.

extensions.cfg:
    List of allowed extensions, each in a new line.
    Example:
        .c
        .h
        .js
        .py

ignore.cfg:
    List of directories to be ignored, each in a new line.
    Example:
        node_modules
        ./doc/en

Both of these files have default values that can be found (and modified) in the load_config_extensions() and load_config_ignore() functions
Default values only apply if config files are damaged or can't be found

If -i flag is set, the tool will ignore the hidden files (.gitignore for example)
