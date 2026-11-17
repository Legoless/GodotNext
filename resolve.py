import re
import sys

def resolve(filename):
    with open(filename, 'r') as f:
        content = f.read()
    
    # Remove all markers and keep the first part (HEAD)
    # The format is <<<<<<< HEAD\nPart1\n=======\nPart2\n>>>>>>> 4f029c823e
    # Pattern to match
    pattern = re.compile(r'<<<<<<< HEAD\n(.*?)\n=======\n.*?\n>>>>>>> [0-9a-f]+', re.DOTALL)
    
    new_content = pattern.sub(r'\1', content)
    
    with open(filename, 'w') as f:
        f.write(new_content)

resolve('servers/rendering/storage/virtual_shadow_map_storage.cpp')
resolve('servers/rendering/storage/virtual_shadow_map_storage.h')
resolve('servers/rendering/renderer_rd/storage_rd/virtual_shadow_map_storage.cpp')
