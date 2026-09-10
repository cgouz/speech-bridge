// Command gguf-kv inspects or patches GGUF metadata. Its one job here is to add
// keys that older converters omitted but the vendored llama.cpp now requires
// (e.g. t5.context_length on 2024-era MADLAD-400 GGUFs). Metadata-only: tensor
// data is copied verbatim, tensor offsets are unchanged.
//
//	gguf-kv get  <file> <key>
//	gguf-kv ensure-u32 <file> <key> <value>   # add only if absent (in place)
package main

import (
	"bufio"
	"encoding/binary"
	"fmt"
	"io"
	"os"
)

const (
	ggufMagic  = 0x46554747 // "GGUF" little-endian
	alignDefau = 32

	typeUint8   = 0
	typeInt8    = 1
	typeUint16  = 2
	typeInt16   = 3
	typeUint32  = 4
	typeInt32   = 5
	typeFloat32 = 6
	typeBool    = 7
	typeString  = 8
	typeArray   = 9
	typeUint64  = 10
	typeInt64   = 11
	typeFloat64 = 12
)

func main() {
	if len(os.Args) < 4 {
		fmt.Fprintln(os.Stderr, "usage: gguf-kv get <file> <key> | gguf-kv ensure-u32 <file> <key> <value>")
		os.Exit(2)
	}
	switch os.Args[1] {
	case "get":
		if v, ok, err := getKey(os.Args[2], os.Args[3]); err != nil {
			die(err)
		} else if !ok {
			fmt.Println("(absent)")
			os.Exit(1)
		} else {
			fmt.Println(v)
		}
	case "ensure-u32":
		if len(os.Args) < 5 {
			die(fmt.Errorf("ensure-u32 needs <file> <key> <value>"))
		}
		var val uint64
		fmt.Sscan(os.Args[4], &val)
		changed, err := ensureU32(os.Args[2], os.Args[3], uint32(val))
		if err != nil {
			die(err)
		}
		if changed {
			fmt.Printf("added %s=%d\n", os.Args[3], val)
		} else {
			fmt.Printf("%s already present; unchanged\n", os.Args[3])
		}
	default:
		die(fmt.Errorf("unknown subcommand %q", os.Args[1]))
	}
}

func die(err error) { fmt.Fprintln(os.Stderr, "gguf-kv:", err); os.Exit(1) }

// ---- GGUF parsing ----

type reader struct {
	r   io.Reader
	n   int64 // bytes consumed
	err error
}

func (rd *reader) read(p []byte) {
	if rd.err != nil {
		return
	}
	m, err := io.ReadFull(rd.r, p)
	rd.n += int64(m)
	rd.err = err
}
func (rd *reader) u32() uint32 { var b [4]byte; rd.read(b[:]); return binary.LittleEndian.Uint32(b[:]) }
func (rd *reader) u64() uint64 { var b [8]byte; rd.read(b[:]); return binary.LittleEndian.Uint64(b[:]) }
func (rd *reader) str() string {
	n := rd.u64()
	if rd.err != nil || n > 1<<30 {
		return ""
	}
	b := make([]byte, n)
	rd.read(b)
	return string(b)
}

// skipValue advances past one metadata value of the given type.
func (rd *reader) skipValue(t uint32) {
	switch t {
	case typeUint8, typeInt8, typeBool:
		var b [1]byte
		rd.read(b[:])
	case typeUint16, typeInt16:
		var b [2]byte
		rd.read(b[:])
	case typeUint32, typeInt32, typeFloat32:
		var b [4]byte
		rd.read(b[:])
	case typeUint64, typeInt64, typeFloat64:
		var b [8]byte
		rd.read(b[:])
	case typeString:
		rd.str()
	case typeArray:
		et := rd.u32()
		n := rd.u64()
		for i := uint64(0); i < n && rd.err == nil; i++ {
			rd.skipValue(et)
		}
	default:
		rd.err = fmt.Errorf("unknown gguf value type %d", t)
	}
}

// header parses everything before the tensor-data section and returns the raw
// header bytes plus the kv count and the byte offset where kv entries begin.
type header struct {
	raw        []byte
	version    uint32
	tensorCnt  uint64
	kvCount    uint64
	kvStart    int   // offset in raw where the first KV begins
	headerEnd  int64 // bytes from file start to end of tensor-info section (before padding)
	alignment  uint64
	keys       map[string]bool
}

func parseHeader(path string) (*header, error) {
	f, err := os.Open(path)
	if err != nil {
		return nil, err
	}
	defer f.Close()

	// Read generously; GGUF headers are well under 8 MiB.
	buf := make([]byte, 8<<20)
	nRead, _ := io.ReadFull(bufio.NewReader(f), buf)
	buf = buf[:nRead]

	rd := &reader{r: newBytesReader(buf)}
	if rd.u32() != ggufMagic {
		return nil, fmt.Errorf("not a GGUF file")
	}
	h := &header{keys: map[string]bool{}, alignment: alignDefau}
	h.version = rd.u32()
	h.tensorCnt = rd.u64()
	h.kvCount = rd.u64()
	h.kvStart = int(rd.n)

	for i := uint64(0); i < h.kvCount && rd.err == nil; i++ {
		key := rd.str()
		t := rd.u32()
		if key == "general.alignment" && t == typeUint32 {
			var b [4]byte
			rd.read(b[:])
			h.alignment = uint64(binary.LittleEndian.Uint32(b[:]))
			h.keys[key] = true
			continue
		}
		h.keys[key] = true
		rd.skipValue(t)
	}
	if rd.err != nil {
		return nil, fmt.Errorf("parsing kv: %w", rd.err)
	}

	// tensor infos
	for i := uint64(0); i < h.tensorCnt && rd.err == nil; i++ {
		rd.str()        // name
		nd := rd.u32()  // n_dims
		for d := uint32(0); d < nd; d++ {
			rd.u64()
		}
		rd.u32() // type
		rd.u64() // offset
	}
	if rd.err != nil {
		return nil, fmt.Errorf("parsing tensor infos: %w", rd.err)
	}
	h.headerEnd = rd.n
	h.raw = buf[:rd.n]
	return h, nil
}

func getKey(path, key string) (string, bool, error) {
	f, err := os.Open(path)
	if err != nil {
		return "", false, err
	}
	defer f.Close()
	rd := &reader{r: bufio.NewReader(f)}
	if rd.u32() != ggufMagic {
		return "", false, fmt.Errorf("not a GGUF file")
	}
	rd.u32()
	rd.u64()
	kvc := rd.u64()
	for i := uint64(0); i < kvc && rd.err == nil; i++ {
		k := rd.str()
		t := rd.u32()
		if k == key {
			return readScalarString(rd, t), true, rd.err
		}
		rd.skipValue(t)
	}
	return "", false, rd.err
}

func readScalarString(rd *reader, t uint32) string {
	switch t {
	case typeUint32, typeInt32:
		return fmt.Sprint(rd.u32())
	case typeUint64, typeInt64:
		return fmt.Sprint(rd.u64())
	case typeString:
		return rd.str()
	default:
		rd.skipValue(t)
		return fmt.Sprintf("(type %d)", t)
	}
}

// ensureU32 adds key=val (uint32) if absent, rewriting the file atomically.
func ensureU32(path, key string, val uint32) (bool, error) {
	h, err := parseHeader(path)
	if err != nil {
		return false, err
	}
	if h.keys[key] {
		return false, nil
	}

	src, err := os.Open(path)
	if err != nil {
		return false, err
	}
	defer src.Close()

	tmp := path + ".tmp"
	dst, err := os.Create(tmp)
	if err != nil {
		return false, err
	}
	w := bufio.NewWriterSize(dst, 1<<20)

	// new header: magic, version, tensorCnt, kvCount+1, [existing kv bytes], [new kv], [tensor infos]
	var head []byte
	head = append(head, le32(ggufMagic)...)
	head = append(head, le32(h.version)...)
	head = append(head, le64(h.tensorCnt)...)
	head = append(head, le64(h.kvCount+1)...)

	// existing kv bytes: from kvStart to the point tensor infos begin. We need
	// that split offset — re-parse to find where KVs end.
	kvEnd, err := kvSectionEnd(h)
	if err != nil {
		dst.Close()
		os.Remove(tmp)
		return false, err
	}
	head = append(head, h.raw[h.kvStart:kvEnd]...)        // existing KVs
	head = append(head, encodeU32KV(key, val)...)          // the new KV
	tensorInfos := h.raw[kvEnd:h.headerEnd]
	head = append(head, tensorInfos...)

	// pad to alignment, matching how gguf lays out the data section
	align := h.alignment
	if align == 0 {
		align = alignDefau
	}
	pad := (align - (uint64(len(head)) % align)) % align
	head = append(head, make([]byte, pad)...)

	if _, err := w.Write(head); err != nil {
		dst.Close()
		os.Remove(tmp)
		return false, err
	}

	// copy tensor data verbatim: it starts in the original file at the padded
	// end of its header.
	origDataStart := alignUp(h.headerEnd, int64(align))
	if _, err := src.Seek(origDataStart, io.SeekStart); err != nil {
		dst.Close()
		os.Remove(tmp)
		return false, err
	}
	if _, err := io.Copy(w, src); err != nil {
		dst.Close()
		os.Remove(tmp)
		return false, err
	}
	if err := w.Flush(); err != nil {
		dst.Close()
		os.Remove(tmp)
		return false, err
	}
	if err := dst.Close(); err != nil {
		os.Remove(tmp)
		return false, err
	}
	if err := os.Rename(tmp, path); err != nil {
		return false, err
	}
	return true, nil
}

// kvSectionEnd re-parses raw to find the byte offset where the KV section ends.
func kvSectionEnd(h *header) (int, error) {
	rd := &reader{r: newBytesReader(h.raw[h.kvStart:])}
	for i := uint64(0); i < h.kvCount && rd.err == nil; i++ {
		rd.str()
		t := rd.u32()
		rd.skipValue(t)
	}
	if rd.err != nil {
		return 0, rd.err
	}
	return h.kvStart + int(rd.n), nil
}

func encodeU32KV(key string, val uint32) []byte {
	var b []byte
	b = append(b, le64(uint64(len(key)))...)
	b = append(b, key...)
	b = append(b, le32(typeUint32)...)
	b = append(b, le32(val)...)
	return b
}

func le32[T ~uint32 | ~int](v T) []byte {
	var b [4]byte
	binary.LittleEndian.PutUint32(b[:], uint32(v))
	return b[:]
}
func le64[T ~uint64 | ~int](v T) []byte {
	var b [8]byte
	binary.LittleEndian.PutUint64(b[:], uint64(v))
	return b[:]
}
func alignUp(n, a int64) int64 { return (n + a - 1) / a * a }

// tiny bytes.Reader without importing bytes for clarity of intent
type bytesReader struct {
	b []byte
	i int
}

func newBytesReader(b []byte) *bytesReader { return &bytesReader{b: b} }
func (r *bytesReader) Read(p []byte) (int, error) {
	if r.i >= len(r.b) {
		return 0, io.EOF
	}
	n := copy(p, r.b[r.i:])
	r.i += n
	return n, nil
}
