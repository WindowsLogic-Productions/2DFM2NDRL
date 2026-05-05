#!/usr/bin/env python3
"""
Convert ArtMoney table to Cheat Engine format with organized groups
"""

import re
import xml.etree.ElementTree as ET

# Known correct addresses
P1_HP_ADDR = 0x004DFC85
P2_HP_ADDR = 0x004EDCC4

# Type mapping
TYPE_MAP = {
    'ni1': 'Byte',
    'ni2': '2 Bytes',
    'ni4': '4 Bytes',
    'R': '4 Bytes',  # Pointer
}

def parse_offset(offset_str):
    """Parse offset like '+6212F', '-E03B', or absolute '0047DB94'"""
    if not offset_str:
        return None
    
    offset_str = offset_str.strip()
    
    # Absolute address (8 hex digits)
    if len(offset_str) == 8 and all(c in '0123456789ABCDEF' for c in offset_str):
        return int(offset_str, 16)
    
    # Relative offset
    sign = 1
    if offset_str.startswith('+'):
        offset_str = offset_str[1:]
    elif offset_str.startswith('-'):
        sign = -1
        offset_str = offset_str[1:]
    
    try:
        return sign * int(offset_str, 16)
    except:
        return None

# Read ArtMoney file (it's all on one line)
with open('FM-Variables.amt', 'r', encoding='utf-8') as f:
    content = f.read().strip()

# Extract all quoted fields
quoted_fields = re.findall(r'"([^"]*)"', content)

# Group into entries of 8 fields each
entries = []
i = 0
while i < len(quoted_fields):
    # Skip header entries until we find "Timer"
    if quoted_fields[i] == 'Timer':
        break
    i += 1

# Now parse entries starting from "Timer"
while i < len(quoted_fields):
    if i + 7 < len(quoted_fields):
        name = quoted_fields[i]
        offset = quoted_fields[i+1]
        type_code = quoted_fields[i+5]
        
        # Skip if invalid
        if not name or not offset or not type_code:
            i += 1
            continue
        
        # Only process entries with valid types
        if type_code in TYPE_MAP or type_code == 'R':
            entries.append((name, offset, type_code))
        
        i += 8
    else:
        i += 1

print(f'Found {len(entries)} entries')

# Character data bases
P1_CHAR_BASE = 0x004DFC00
P2_CHAR_BASE = 0x004EDC00
SYSTEM_VARS_BASE = 0x004456B0

def calculate_address(name, offset_str, type_code):
    """Calculate absolute address"""
    # Known addresses
    if name == 'Timer':
        return 0x0047DB94
    if name == 'P1 HP':
        return P1_HP_ADDR
    if name == 'P2 HP':
        return P2_HP_ADDR
    if name == 'Round Number':
        return 0x00470044
    
    # Determine base
    if 'P1' in name:
        base = P1_CHAR_BASE
    elif 'P2' in name:
        base = P2_CHAR_BASE
    elif 'System Var' in name:
        base = SYSTEM_VARS_BASE
    elif 'Task Var' in name:
        base = 0x00470000
    else:
        base = 0x00470000
    
    offset = parse_offset(offset_str)
    if offset is None:
        return None
    
    # If absolute address, return it
    if offset_str and len(offset_str) == 8 and all(c in '0123456789ABCDEF' for c in offset_str):
        return offset
    
    return base + offset

def categorize_entry(name):
    """Categorize entry into a group"""
    if name == 'Timer' or name == 'Round Number':
        return 'General'
    elif 'P1 HP' in name or 'P1 Super' in name or 'P1 Special Stock' in name:
        return 'Player 1 Stats'
    elif 'P2 HP' in name or 'P2 Super' in name or 'P2 Special Stock' in name:
        return 'Player 2 Stats'
    elif 'Coor' in name or 'Map' in name:
        return 'Coordinates'
    elif 'Char Var' in name and 'P1' in name:
        return 'Player 1 Character Variables'
    elif 'Char Var' in name and 'P2' in name:
        return 'Player 2 Character Variables'
    elif 'System Var' in name:
        return 'System Variables'
    elif 'Task Var' in name and 'P1' in name:
        return 'Player 1 Task Variables'
    elif 'Task Var' in name and 'P2' in name:
        return 'Player 2 Task Variables'
    elif 'Parent' in name:
        return 'Parent Variables'
    else:
        return 'Other'

# Build XML with groups
root = ET.Element('CheatTable')
root.set('CheatEngineTableVersion', '44')
cheat_entries = ET.SubElement(root, 'CheatEntries')

# Organize entries by category
categories = {}
for name, offset_str, type_code in entries:
    category = categorize_entry(name)
    if category not in categories:
        categories[category] = []
    categories[category].append((name, offset_str, type_code))

# Define group order
group_order = [
    'General',
    'Player 1 Stats',
    'Player 2 Stats',
    'Coordinates',
    'Player 1 Character Variables',
    'Player 2 Character Variables',
    'System Variables',
    'Player 1 Task Variables',
    'Player 2 Task Variables',
    'Parent Variables',
    'Other'
]

entry_id = 1
for group_name in group_order:
    if group_name not in categories:
        continue
    
    # Create group header
    group_entry = ET.SubElement(cheat_entries, 'CheatEntry')
    ET.SubElement(group_entry, 'ID').text = str(entry_id)
    ET.SubElement(group_entry, 'Description').text = group_name
    ET.SubElement(group_entry, 'Options').set('moHideChildren', '1')
    ET.SubElement(group_entry, 'GroupHeader').text = '1'
    
    group_entries = ET.SubElement(group_entry, 'CheatEntries')
    entry_id += 1
    
    # Add entries to group
    for name, offset_str, type_code in categories[group_name]:
        addr = calculate_address(name, offset_str, type_code)
        if addr is None:
            print(f'Skipping {name}: could not calculate address (offset: {offset_str})')
            continue
        
        entry = ET.SubElement(group_entries, 'CheatEntry')
        ET.SubElement(entry, 'ID').text = str(entry_id)
        ET.SubElement(entry, 'Description').text = name
        ET.SubElement(entry, 'ShowAsSigned').text = '0'
        
        var_type = TYPE_MAP.get(type_code, '4 Bytes')
        ET.SubElement(entry, 'VariableType').text = var_type
        
        addr_str = f'{addr:08X}'
        ET.SubElement(entry, 'Address').text = addr_str
        
        entry_id += 1

# Write XML
tree = ET.ElementTree(root)
ET.indent(tree, space='  ')
tree.write('FM-Variables.CT', encoding='utf-8', xml_declaration=True)
print(f'Created FM-Variables.CT with {len(entries)} entries organized into {len(categories)} groups')
