#!/usr/bin/env python3

import sys
import argparse
import os
import re

line_ending= b'\n'

def read_file(file_path):
    print("read file %s" % file_path)
    fw = open(file_path, 'rb')
    return fw.read()

def write_file(file_path, body):
    fw = open(file_path, 'wb')
    fw.write(body)
    print("update file %s" % file_path)

def compare_body(origin_body, target_body):
    return origin_body == target_body

def replace(file_path, target_head):
    head_start = False
    head_end = False
    first_line = False
    body = b''
    with open(file_path, 'rb') as fr:
        for lineBin in fr:
            lineStr = lineBin.decode('utf-8')
            # Detect start line in head
            if not head_start:
                head_start = re.match(r'^\/\*\*\*+', lineStr)
                if head_start:
                    continue
                else:
                    head_start = True
                    head_end = True
            # Detect end line in head
            if head_start and not head_end:
                head_end = re.match(r'^\s*\*\*\*+\/', lineStr)
                continue
            # Wait first line in body
            if not first_line:
                if lineStr == '' or lineStr == '\n' or lineStr == '\r\n':
                    continue
                first_line = True
            # Add line by line
            body += lineBin
    # Output with head and body
    output = target_head + line_ending + body
    return output

def replace_in_path(path, head_file, apply_needed):
    result = 0
    target_head = read_file(head_file)
    print("search files in %s" % path)
    for root, dirs, files in os.walk(path):
        for file in files:
            if (file.endswith('.h') or file.endswith('.cpp')) and \
                not file.startswith('moc') and \
                not 'build/' in root :
                    file_path = root + "/" + file
                    origin_body = read_file(file_path)
                    target_body = replace(file_path, target_head)
                    if not compare_body(origin_body, target_body):
                        print("HEAD DIFFER IN %s" % file_path)
                        result = 1
                        if apply_needed:
                            write_file(file_path, target_body)
    return result

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("path", help="path to project")
    parser.add_argument("head_file", help="path to head file")
    parser.add_argument('--apply', action='store_true')
    args = parser.parse_args()
    print("Replace in '%s' Target head file '%s'" % (args.path, args.head_file))
    print(args)
    return replace_in_path(args.path, args.head_file, args.apply)

if __name__ == "__main__":
    sys.exit(main())
