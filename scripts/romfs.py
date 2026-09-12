#!/usr/bin/env python3
import argparse
import os
import stat
import struct
import sys

MAGIC = b"-rom1fs-"
SUPERBLOCK_SIZE = 16
ALIGN = 16
FS_PAD = 1024

TYPE_HARDLINK = 0
TYPE_DIR = 1
TYPE_FILE = 2
TYPE_SYMLINK = 3
TYPE_BLOCK = 4
TYPE_CHAR = 5
TYPE_SOCKET = 6
TYPE_FIFO = 7


def align_up(value, align):
    return (value + (align - 1)) & ~(align - 1)


def be32(value):
    return struct.pack(">I", value & 0xFFFFFFFF)


def sum_be32_words(data):
    if len(data) % 4 != 0:
        raise ValueError("data length must be multiple of 4 bytes")
    total = 0
    for i in range(0, len(data), 4):
        total = (total + struct.unpack(">I", data[i:i + 4])[0]) & 0xFFFFFFFF
    return total


def checksum_block(data):
    total = sum_be32_words(data)
    return (-total) & 0xFFFFFFFF


class Node:
    def __init__(self, name, node_type, st=None):
        self.name = name
        self.node_type = node_type
        self.st = st
        self.exec_bit = False
        self.data = b""
        self.children = []
        self.link_target = None
        self.spec_info = 0
        self.size = 0
        self.offset = None

    def name_bytes(self):
        return self.name.encode("utf-8")

    def header_meta_len(self):
        name_len = len(self.name_bytes()) + 1
        return SUPERBLOCK_SIZE + align_up(name_len, ALIGN)

    def data_len(self):
        if self.node_type in (TYPE_FILE, TYPE_SYMLINK):
            return self.size
        return 0

    def total_len(self):
        return self.header_meta_len() + align_up(self.data_len(), ALIGN)


def build_tree(root_dir):
    inode_map = {}

    def build_node(path, name, parent):
        st = os.lstat(path)
        mode = st.st_mode
        exec_bit = bool(mode & 0o111)

        if stat.S_ISDIR(mode):
            node = Node(name, TYPE_DIR, st=st)
            node.exec_bit = exec_bit
            entries = []
            with os.scandir(path) as it:
                for entry in it:
                    entries.append(entry.name)
            entries.sort()
            # "." and ".." are hardlinks to current and parent directories.
            dot = Node(".", TYPE_HARDLINK)
            dot.link_target = node
            dotdot = Node("..", TYPE_HARDLINK)
            dotdot.link_target = parent if parent is not None else node
            node.children.extend([dot, dotdot])
            for entry_name in entries:
                child_path = os.path.join(path, entry_name)
                node.children.append(build_node(child_path, entry_name, node))
            return node

        if stat.S_ISLNK(mode):
            node = Node(name, TYPE_SYMLINK, st=st)
            node.exec_bit = exec_bit
            target = os.readlink(path)
            node.data = target.encode("utf-8")
            node.size = len(node.data)
            return node

        if stat.S_ISREG(mode):
            inode_key = (st.st_dev, st.st_ino)
            if inode_key in inode_map:
                link = Node(name, TYPE_HARDLINK, st=st)
                link.link_target = inode_map[inode_key]
                return link
            node = Node(name, TYPE_FILE, st=st)
            node.exec_bit = exec_bit
            with open(path, "rb") as f:
                node.data = f.read()
            node.size = len(node.data)
            inode_map[inode_key] = node
            return node

        if stat.S_ISBLK(mode):
            node = Node(name, TYPE_BLOCK, st=st)
            node.exec_bit = exec_bit
            node.spec_info = (os.major(st.st_rdev) << 16) | os.minor(st.st_rdev)
            return node

        if stat.S_ISCHR(mode):
            node = Node(name, TYPE_CHAR, st=st)
            node.exec_bit = exec_bit
            node.spec_info = (os.major(st.st_rdev) << 16) | os.minor(st.st_rdev)
            return node

        if stat.S_ISSOCK(mode):
            node = Node(name, TYPE_SOCKET, st=st)
            node.exec_bit = exec_bit
            return node

        if stat.S_ISFIFO(mode):
            node = Node(name, TYPE_FIFO, st=st)
            node.exec_bit = exec_bit
            return node

        raise ValueError(f"unsupported file type: {path}")

    root = Node("", TYPE_DIR)
    root.exec_bit = True
    entries = []
    with os.scandir(root_dir) as it:
        for entry in it:
            entries.append(entry.name)
    entries.sort()
    dot = Node(".", TYPE_HARDLINK)
    dot.link_target = root
    dotdot = Node("..", TYPE_HARDLINK)
    dotdot.link_target = root
    root.children.extend([dot, dotdot])
    for entry_name in entries:
        child_path = os.path.join(root_dir, entry_name)
        root.children.append(build_node(child_path, entry_name, root))
    return root


def assign_offsets(node, start_offset):
    node.offset = start_offset
    next_offset = start_offset + node.total_len()
    if node.node_type == TYPE_DIR:
        for child in node.children:
            next_offset = assign_offsets(child, next_offset)
    return next_offset


def write_node(node, image, next_offset):
    if node.link_target is not None:
        node.spec_info = node.link_target.offset
    elif node.node_type == TYPE_DIR:
        node.spec_info = node.children[0].offset if node.children else 0

    type_bits = node.node_type & 0x7
    mode_bits = type_bits | (0x8 if node.exec_bit else 0x0)
    next_field = (next_offset or 0) | mode_bits
    header = [
        be32(next_field),
        be32(node.spec_info),
        be32(node.size),
        be32(0),
    ]
    name_bytes = node.name_bytes() + b"\x00"
    name_padded = name_bytes + b"\x00" * (align_up(len(name_bytes), ALIGN) - len(name_bytes))
    meta = b"".join(header) + name_padded
    chksum = checksum_block(meta)
    meta = meta[:12] + be32(chksum) + meta[16:]
    image.extend(meta)
    if node.data_len():
        data = node.data
        image.extend(data)
        image.extend(b"\x00" * (align_up(len(data), ALIGN) - len(data)))


def serialize(root, volume_name):
    volume_bytes = volume_name.encode("utf-8") + b"\x00"
    volume_padded = volume_bytes + b"\x00" * (align_up(len(volume_bytes), ALIGN) - len(volume_bytes))
    start_offset = SUPERBLOCK_SIZE + len(volume_padded)

    end_offset = assign_offsets(root, start_offset)
    full_size = align_up(end_offset, FS_PAD)

    image = bytearray()
    image.extend(MAGIC)
    image.extend(be32(full_size))
    image.extend(be32(0))
    image.extend(volume_padded)

    def emit_dir(node):
        if node.node_type != TYPE_DIR:
            return
        for i, child in enumerate(node.children):
            next_child = node.children[i + 1] if i + 1 < len(node.children) else None
            next_offset = next_child.offset if next_child else 0
            write_node(child, image, next_offset)
            if child.node_type == TYPE_DIR:
                emit_dir(child)

    write_node(root, image, 0)
    emit_dir(root)

    if len(image) < full_size:
        image.extend(b"\x00" * (full_size - len(image)))

    checksum_len = min(512, full_size)
    checksum_data = bytes(image[:checksum_len])
    chksum = checksum_block(checksum_data)
    image[12:16] = be32(chksum)
    return image


def read_u32(data, offset):
    if offset + 4 > len(data):
        raise ValueError("unexpected end of image")
    return struct.unpack(">I", data[offset:offset + 4])[0]


def parse_superblock(data):
    if len(data) < SUPERBLOCK_SIZE:
        raise ValueError("image too small")
    if data[:8] != MAGIC:
        raise ValueError("invalid romfs magic")
    full_size = read_u32(data, 8)
    if full_size == 0 or full_size > len(data):
        raise ValueError("invalid romfs size")
    if full_size % 4 != 0:
        raise ValueError("invalid romfs size alignment")
    checksum_len = min(512, full_size)
    if checksum_len % 4 != 0:
        raise ValueError("invalid checksum region size")
    if sum_be32_words(data[:checksum_len]) != 0:
        raise ValueError("romfs superblock checksum mismatch")
    name_start = SUPERBLOCK_SIZE
    name_end = data.find(b"\x00", name_start, full_size)
    if name_end == -1:
        raise ValueError("volume name not terminated")
    volume_name = data[name_start:name_end].decode("utf-8", errors="replace")
    name_padded_len = align_up((name_end - name_start) + 1, ALIGN)
    root_offset = SUPERBLOCK_SIZE + name_padded_len
    if root_offset % ALIGN != 0:
        raise ValueError("root offset not aligned")
    return full_size, volume_name, root_offset


def parse_header(data, offset, full_size):
    if offset % ALIGN != 0:
        raise ValueError(f"unaligned header at {offset}")
    if offset + SUPERBLOCK_SIZE > full_size:
        raise ValueError("header exceeds image size")
    next_field = read_u32(data, offset)
    spec_info = read_u32(data, offset + 4)
    size = read_u32(data, offset + 8)
    checksum = read_u32(data, offset + 12)
    name_start = offset + SUPERBLOCK_SIZE
    name_end = data.find(b"\x00", name_start, full_size)
    if name_end == -1:
        raise ValueError("unterminated name")
    name = data[name_start:name_end].decode("utf-8", errors="replace")
    name_padded_len = align_up((name_end - name_start) + 1, ALIGN)
    meta_len = SUPERBLOCK_SIZE + name_padded_len
    if offset + meta_len > full_size:
        raise ValueError("header metadata exceeds image size")
    meta = data[offset:offset + meta_len]
    if sum_be32_words(meta) != 0:
        raise ValueError(f"header checksum mismatch at {offset}")
    mode_bits = next_field & 0xF
    next_offset = next_field & ~0xF
    node_type = mode_bits & 0x7
    exec_bit = bool(mode_bits & 0x8)
    data_offset = offset + meta_len
    data_offset = offset + meta_len
    if data_offset + size > full_size:
        raise ValueError("file data exceeds image size")
    return {
        "offset": offset,
        "next_offset": next_offset,
        "spec_info": spec_info,
        "size": size,
        "checksum": checksum,
        "name": name,
        "node_type": node_type,
        "exec_bit": exec_bit,
        "meta_len": meta_len,
        "data_offset": data_offset,
    }


def traverse_directory(data, start_offset, full_size, base_path, entries, visited):
    offset = start_offset
    while offset:
        if offset in visited:
            raise ValueError(f"loop detected at {offset}")
        visited.add(offset)
        if offset >= full_size:
            raise ValueError(f"header offset out of bounds: {offset}")
        info = parse_header(data, offset, full_size)
        name = info["name"]
        node_type = info["node_type"]
        entry_path = os.path.join(base_path, name) if name else base_path
        entries.append((entry_path, info))
        if node_type == TYPE_DIR:
            child_offset = info["spec_info"]
            if child_offset and (child_offset % ALIGN != 0 or child_offset >= full_size):
                raise ValueError(f"invalid directory entry offset: {child_offset}")
            if name not in (".", ".."):
                traverse_directory(data, child_offset, full_size, entry_path, entries, visited)
        next_offset = info["next_offset"]
        if next_offset and (next_offset % ALIGN != 0 or next_offset >= full_size):
            raise ValueError(f"invalid next offset: {next_offset}")
        offset = next_offset


def list_image(image_path):
    with open(image_path, "rb") as f:
        data = f.read()
    full_size, volume_name, root_offset = parse_superblock(data)
    entries = []
    visited = set()
    root_info = parse_header(data, root_offset, full_size)
    if root_info["node_type"] != TYPE_DIR:
        raise ValueError("root node is not a directory")
    entries.append(("/", root_info))
    traverse_directory(data, root_info["spec_info"], full_size, "/", entries, visited)
    print(f"Volume: {volume_name}")
    for path, info in entries:
        if os.path.basename(path) in (".", ".."):
            continue
        node_type = info["node_type"]
        size = info["size"]
        if node_type == TYPE_DIR:
            kind = "dir"
        elif node_type == TYPE_FILE:
            kind = "file"
        elif node_type == TYPE_SYMLINK:
            kind = "symlink"
        elif node_type == TYPE_HARDLINK:
            kind = "hardlink"
        elif node_type == TYPE_BLOCK:
            kind = "block"
        elif node_type == TYPE_CHAR:
            kind = "char"
        elif node_type == TYPE_SOCKET:
            kind = "socket"
        else:
            kind = "fifo"
        line = f"0x{info['offset']:08x} {kind:8} {size:10} {path}"
        if node_type == TYPE_SYMLINK:
            target = data[info["data_offset"]:info["data_offset"] + size].decode("utf-8", errors="replace")
            line += f" -> {target}"
        print(line)


def extract_image(image_path, output_dir):
    with open(image_path, "rb") as f:
        data = f.read()
    full_size, volume_name, root_offset = parse_superblock(data)
    os.makedirs(output_dir, exist_ok=True)
    entries = []
    visited = set()
    root_info = parse_header(data, root_offset, full_size)
    if root_info["node_type"] != TYPE_DIR:
        raise ValueError("root node is not a directory")
    traverse_directory(data, root_info["spec_info"], full_size, output_dir, entries, visited)
    hardlinks = []
    offset_to_path = {}

    for path, info in entries:
        node_type = info["node_type"]
        name = os.path.basename(path)
        if name in (".", ".."):
            continue
        if name == "" or os.path.sep in name or (os.path.altsep and os.path.altsep in name):
            raise ValueError(f"invalid entry name: {name!r}")
        offset_to_path[info["offset"]] = path
        if node_type == TYPE_DIR:
            os.makedirs(path, exist_ok=True)
        elif node_type == TYPE_FILE:
            os.makedirs(os.path.dirname(path), exist_ok=True)
            start = info["data_offset"]
            end = start + info["size"]
            with open(path, "wb") as f:
                f.write(data[start:end])
        elif node_type == TYPE_SYMLINK:
            os.makedirs(os.path.dirname(path), exist_ok=True)
            start = info["data_offset"]
            end = start + info["size"]
            target = data[start:end].decode("utf-8", errors="replace")
            os.symlink(target, path)
        elif node_type == TYPE_HARDLINK:
            hardlinks.append((path, info["spec_info"]))
        elif node_type == TYPE_BLOCK:
            os.makedirs(os.path.dirname(path), exist_ok=True)
            os.mknod(path, stat.S_IFBLK | 0o600, info["spec_info"])
        elif node_type == TYPE_CHAR:
            os.makedirs(os.path.dirname(path), exist_ok=True)
            os.mknod(path, stat.S_IFCHR | 0o600, info["spec_info"])
        elif node_type == TYPE_FIFO:
            os.makedirs(os.path.dirname(path), exist_ok=True)
            os.mkfifo(path)
        elif node_type == TYPE_SOCKET:
            continue
        else:
            raise ValueError(f"unsupported node type {node_type}")

    for path, target_offset in hardlinks:
        name = os.path.basename(path)
        if name in (".", ".."):
            continue
        target_path = offset_to_path.get(target_offset)
        if target_path is None:
            raise ValueError(f"hardlink target not found at {target_offset}")
        os.makedirs(os.path.dirname(path), exist_ok=True)
        os.link(target_path, path)


def pack_image(input_dir, image_path, volume_name):
    root = build_tree(input_dir)
    image = serialize(root, volume_name)
    with open(image_path, "wb") as f:
        f.write(image)


def main(argv=None):
    parser = argparse.ArgumentParser(description="ROMFS pack/list/extract tool")
    subparsers = parser.add_subparsers(dest="command", required=True)

    pack_parser = subparsers.add_parser("pack", help="pack a directory into ROMFS image")
    pack_parser.add_argument("-i", "--input", required=True, help="input directory")
    pack_parser.add_argument("-o", "--output", required=True, help="output image path")
    pack_parser.add_argument("-n", "--name", default="romfs", help="volume name")

    list_parser = subparsers.add_parser("list", help="list contents of ROMFS image")
    list_parser.add_argument("-i", "--input", required=True, help="input image path")

    extract_parser = subparsers.add_parser("extract", help="extract ROMFS image")
    extract_parser.add_argument("-i", "--input", required=True, help="input image path")
    extract_parser.add_argument("-o", "--output", required=True, help="output directory")

    args = parser.parse_args(argv)

    if args.command == "pack":
        pack_image(args.input, args.output, args.name)
    elif args.command == "list":
        list_image(args.input)
    elif args.command == "extract":
        extract_image(args.input, args.output)
    else:
        parser.error("unknown command")


if __name__ == "__main__":
    main()
