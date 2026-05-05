#!/usr/bin/env python3
"""
Convert ArtMoney table (.amt) to Cheat Engine table (.ct)
"""

import re
import sys
from xml.etree.ElementTree import Element, SubElement, tostring
from xml.dom import minidom

def parse_amt_file(filepath):
    """Parse ArtMoney table file and extract entries."""
    with open(filepath, 'r') as f:
        content = f.read()

    # Split by comma, handling quoted strings
    parts = []
    current = ""
    in_quotes = False
    for char in content:
        if char == '"':
            in_quotes = not in_quotes
        elif char == ',' and not in_quotes:
            parts.append(current.strip('"'))
            current = ""
            continue
        current += char
    if current:
        parts.append(current.strip('"'))

    # Find where entries start (after header)
    # Header format: "ArtMoney Table", version info, process info, etc.
    # Entries start with name, address, empty, "1", empty, type, empty, "0"

    entries = []
    i = 0

    # Skip header - find first entry by looking for pattern
    # Entry pattern: Name, Address, "", "1", "", Type, "", "0"
    while i < len(parts):
        if parts[i] == "Timer":  # First entry name
            break
        i += 1

    # Parse entries (8 fields each: name, addr, ?, "1", ?, type, ?, "0")
    current_addr = 0
    while i + 7 < len(parts):
        name = parts[i]
        addr_str = parts[i + 1]
        type_code = parts[i + 5]

        # Calculate address
        if addr_str.startswith('+'):
            offset = int(addr_str[1:], 16)
            current_addr += offset
        elif addr_str.startswith('-'):
            offset = int(addr_str[1:], 16)
            current_addr -= offset
        else:
            # Absolute address
            current_addr = int(addr_str, 16)

        # Map ArtMoney type to Cheat Engine type
        if type_code == 'ni1':
            ce_type = 'Byte'
            ce_type_id = '0'
        elif type_code == 'ni2':
            ce_type = '2 Bytes'
            ce_type_id = '1'
        elif type_code == 'ni4':
            ce_type = '4 Bytes'
            ce_type_id = '2'
        elif type_code == 'R':
            ce_type = '4 Bytes'  # Pointer displayed as 4 bytes
            ce_type_id = '2'
        else:
            ce_type = '4 Bytes'
            ce_type_id = '2'

        entries.append({
            'name': name,
            'address': current_addr,
            'type': ce_type,
            'type_id': ce_type_id,
            'is_pointer': type_code == 'R'
        })

        i += 8

    return entries

def create_cheat_table(entries, process_name="KGT2nd_GAME.exe"):
    """Create Cheat Engine XML table from entries."""
    root = Element('CheatTable')
    root.set('CheatEngineTableVersion', '44')

    cheat_entries = SubElement(root, 'CheatEntries')

    # Group entries by category
    categories = {}
    current_category = "General"

    for i, entry in enumerate(entries):
        name = entry['name']

        # Detect category from name
        if 'Char Var' in name:
            cat = "Character Variables"
        elif 'System Var' in name:
            cat = "System Variables"
        elif 'Task Var' in name:
            cat = "Task Variables"
        elif 'HP' in name or 'Super' in name or 'Special' in name:
            cat = "Player Stats"
        elif 'Coor' in name or 'Map' in name:
            cat = "Coordinates"
        elif 'Parent' in name:
            cat = "Parent Data"
        else:
            cat = "General"

        if cat not in categories:
            categories[cat] = []
        categories[cat].append((i, entry))

    entry_id = 0

    for cat_name, cat_entries in categories.items():
        # Create category group
        group = SubElement(cheat_entries, 'CheatEntry')
        SubElement(group, 'ID').text = str(entry_id)
        entry_id += 1
        SubElement(group, 'Description').text = f'"{cat_name}"'
        SubElement(group, 'Options').set('moHideChildren', '1')
        SubElement(group, 'GroupHeader').text = '1'

        group_entries = SubElement(group, 'CheatEntries')

        for _, entry in cat_entries:
            ce = SubElement(group_entries, 'CheatEntry')
            SubElement(ce, 'ID').text = str(entry_id)
            entry_id += 1

            desc = entry['name']
            if entry['is_pointer']:
                desc += ' [PTR]'
            SubElement(ce, 'Description').text = f'"{desc}"'
            SubElement(ce, 'VariableType').text = entry['type']
            SubElement(ce, 'Address').text = f"{entry['address']:08X}"

    return root

def prettify(elem):
    """Return a pretty-printed XML string."""
    rough_string = tostring(elem, encoding='unicode')
    reparsed = minidom.parseString(rough_string)
    return reparsed.toprettyxml(indent="  ")

def main():
    input_file = sys.argv[1] if len(sys.argv) > 1 else "FM-Variables.amt"
    output_file = sys.argv[2] if len(sys.argv) > 2 else "FM-Variables.CT"

    print(f"Parsing {input_file}...")
    entries = parse_amt_file(input_file)
    print(f"Found {len(entries)} entries")

    print("Creating Cheat Engine table...")
    root = create_cheat_table(entries)

    xml_str = prettify(root)

    # Fix XML declaration
    xml_str = '<?xml version="1.0" encoding="utf-8"?>\n' + '\n'.join(xml_str.split('\n')[1:])

    with open(output_file, 'w', encoding='utf-8') as f:
        f.write(xml_str)

    print(f"Saved to {output_file}")

    # Print summary
    print("\nEntry summary:")
    for entry in entries[:10]:
        print(f"  {entry['name']}: 0x{entry['address']:08X} ({entry['type']})")
    if len(entries) > 10:
        print(f"  ... and {len(entries) - 10} more entries")

if __name__ == '__main__':
    main()
