package main

import (
	"io"
	"bytes"
	"encoding/json"
)

func toPosition(x, y, z int) uint64 {
	return (uint64(x) << 38) | (uint64(y) << 26) | uint64(z)
}

func fromPosition(position uint64) (int, int, int) {
	return int(position >> 38), int((position >> 26) & 0xFFF), int(position & 0x3FFFFFF)
}

func writeLoginSuccess(w io.Writer, uuid string, username string) error {
	var packet bytes.Buffer

	writeByte(&packet, 0x02)
	writeString(&packet, uuid)
	writeString(&packet, username)

	return writePacket(w, packet.Bytes())
}

func writeJoinGame(w io.Writer, eid int32, gamemode byte, dimension byte, difficulty byte, maxPlayers byte, levelType string, reducedDebugInfo bool) error {
	var packet bytes.Buffer

	writeByte(&packet, 0x01)
	writeInt(&packet, eid)
	writeByte(&packet, gamemode)
	writeByte(&packet, dimension)
	writeByte(&packet, difficulty)
	writeByte(&packet, maxPlayers)
	writeString(&packet, levelType)
	writeBool(&packet, reducedDebugInfo)

	return writePacket(w, packet.Bytes())
}

func writePositionLook(w io.Writer, x, y, z float64, yaw, pitch float32, flags byte) error {
	var packet bytes.Buffer

	writeByte(&packet, 0x08)
	writeDouble(&packet, x)
	writeDouble(&packet, y)
	writeDouble(&packet, z)
	writeFloat(&packet, yaw)
	writeFloat(&packet, pitch)
	writeByte(&packet, flags)

	return writePacket(w, packet.Bytes())
}

func writeEmptyChunk(w io.Writer, x, z int32) error {
	var packet bytes.Buffer

	writeByte(&packet, 0x21)
	writeInt(&packet, x)
	writeInt(&packet, z)
	writeBool(&packet, true)
	writeUnsignedShort(&packet, 0xFFFF)
	writeVarInt(&packet, 0)

	return writePacket(w, packet.Bytes())
}

func writeBlockChangeOld(w io.Writer, x, y, z int, block byte, data byte) error {
	var packet bytes.Buffer

	writeByte(&packet, 0x23)
	writeUnsignedLong(&packet, toPosition(x, y, z))
	writeVarInt(&packet, int32(uint32(block) << 4 | uint32(data)))

	return writePacket(w, packet.Bytes())
}

func writeBlockChange(w io.Writer, x, y, z int, block byte, data byte) error {
	var packet bytes.Buffer

	writeByte(&packet, 0x22)
	writeInt(&packet, int32(x / 16))
	writeInt(&packet, int32(z / 16))
	writeVarInt(&packet, 2)
	writeByte(&packet, byte(x << 4) | byte(z & 0x0F))
	writeByte(&packet, byte(y))
	writeVarInt(&packet, 0)
	writeByte(&packet, byte(x << 4) | byte(z & 0x0F))
	writeByte(&packet, byte(y))
	writeVarInt(&packet, int32(uint32(block) << 4 | uint32(data)))

	return writePacket(w, packet.Bytes())
}

func writeChatMessageRaw(w io.Writer, message string) error {
	var packet bytes.Buffer

	writeByte(&packet, 0x02)

	data := []map[string]string{{"text": message}}
	buffer, _ := json.Marshal(data)

	writeString(&packet, string(buffer))
	writeByte(&packet, 0)

	return writePacket(w, packet.Bytes())
}

func writeChatMessage(w io.Writer, name string, message string) error {
	var packet bytes.Buffer

	writeByte(&packet, 0x02)

	data := []map[string]string{
		{"text": name, "color": "gray"},
		{"text": " » ", "color": "dark_gray"},
		{"text": message, "color": "white"},
	}

	buffer, _ := json.Marshal(data)

	writeString(&packet, string(buffer))
	writeByte(&packet, 0)

	return writePacket(w, packet.Bytes())
}
