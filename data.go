package main

import (
	"io"
	"errors"
	"bytes"
	"encoding/binary"
)

func read[T any](r io.Reader) (result T, err error) {
    err = binary.Read(r, binary.BigEndian, &result)
    return
}

func write(w io.Writer, value any) error {
	return binary.Write(w, binary.BigEndian, value)
}

func readByte(r io.Reader) (result byte, err error) {
	err = binary.Read(r, binary.BigEndian, &result)
	return
}

func writeByte(w io.Writer, value byte) (err error) {
	_, err = w.Write([]byte{value})
	return
}

func readBool(r io.Reader) (result bool, err error) {
	err = binary.Read(r, binary.BigEndian, &result)
	return
}

func writeBool(w io.Writer, value bool) (err error) {
	if (value) {
		return writeByte(w, 1)
	} else {
		return writeByte(w, 0)
	}
}

func writeUnsignedShort(w io.Writer, value uint16) error {
	return binary.Write(w, binary.BigEndian, value)
}

func writeInt(w io.Writer, value int32) error {
	return binary.Write(w, binary.BigEndian, value)
}

func writeUnsignedLong(w io.Writer, value uint64) error {
	return binary.Write(w, binary.BigEndian, value)
}

func readDouble(r io.Reader) (result float64, err error) {
	err = binary.Read(r, binary.BigEndian, &result)
	return
}

func writeDouble(w io.Writer, value float64) error {
	return binary.Write(w, binary.BigEndian, value)
}

func readFloat(r io.Reader) (result float32, err error) {
	err = binary.Read(r, binary.BigEndian, &result)
	return
}

func writeFloat(w io.Writer, value float32) error {
	return binary.Write(w, binary.BigEndian, value)
}

func readSequence(r io.Reader, values ...any) error {
	for _, value := range values {
		if err := binary.Read(r, binary.BigEndian, &value); err != nil {
			return err
		}
	}

	return nil
}

func readVarInt(r io.Reader) (int32, error) {
	var result uint32

	for shift := 0; true; shift += 7 {
		if shift > 28 {
			return 0, errors.New("VarInt too long")
		}

		b, err := readByte(r)

		if err != nil {
			return 0, err
		}

		result |= uint32(b & 0x7F) << shift

		if b & 0x80 == 0 {
			break
		}
	}

	return int32(result), nil
}

func writeVarInt(w io.Writer, value int32) error {
	n := uint32(value)

	if n == 0 {
		return writeByte(w, 0)
	}

	for n != 0 {
		b := byte(n & 0x7F)
		n >>= 7

		if n != 0 {
			b |= 0x80
		}

		if err := writeByte(w, b); err != nil {
			return err
		}
	}

	return nil
}

func readString(r io.Reader) (string, error) {
	length, err := readVarInt(r)

	if err != nil {
		return "", err
	}

	if length > 32767 {
		return "", errors.New("String is too long")
	}

	buf := make([]byte, length)

	if _, err := io.ReadFull(r, buf); err != nil {
		return "", err
	}

	return string(buf), nil
}

func writeString(w io.Writer, value string) error {
	buf := []byte(value)

	if err := writeVarInt(w, int32(len(buf))); err != nil {
		return err;
	}

	if _, err := w.Write(buf); err != nil {
		return err
	}

	return nil
}

func readPacket(r io.Reader) ([]byte, error) {
	length, err := readVarInt(r)

	if err != nil {
		return nil, err
	}

	if length > 0x1FFFFF {
		return nil, errors.New("Packet too long")
	}

	if length <= 0 {
		return nil, errors.New("Packet length nonpositive")
	}

	data := make([]byte, length)

	if _, err := io.ReadFull(r, data); err != nil {
		return nil, err
	}

	return data, nil
}

func readPacketReader(r io.Reader) (*bytes.Reader, error) {
	packet, err := readPacket(r)

	if err != nil {
		return nil, err
	}

	return bytes.NewReader(packet), nil
}

func writePacket(w io.Writer, data []byte) error {
	// TODO: optimize

	var intermediate bytes.Buffer

	writeVarInt(&intermediate, int32(len(data)))
	intermediate.Write(data)

	if _, err := w.Write(intermediate.Bytes()); err != nil {
		return err
	}

	return nil
}
