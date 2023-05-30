package main

import (
	"bytes"
	"encoding/json"
	"log"
	"net"
	"sync"
)

type Player struct {
	// do not change
	conn  net.Conn
	queue chan []byte
	name  string

	x        float64
	y        float64
	z        float64
	yaw      float32
	pitch    float32
	grounded bool
}

var players map[string]*Player = make(map[string]*Player)
var playersLock sync.RWMutex

func broadcastPacket(packet []byte) {
	defer playersLock.RUnlock()
	playersLock.RLock()

	for _, player := range players {
		player.queue <- packet
	}
}

func handleConnection(conn net.Conn) {
	var player Player

	defer func() {
		conn.Close()

		if player.conn != nil {
			close(player.queue)

			playersLock.Lock()
			delete(players, player.name)
			playersLock.Unlock()

			var quitMessage bytes.Buffer
			writeChatMessageRaw(&quitMessage, "\u00a78[\u00a7b-\u00a78] \u00a77"+player.name)
			broadcastPacket(quitMessage.Bytes())
		}
	}()

	handshake, err := readPacket(conn)

	if err != nil {
		log.Println("error reading handshake:", err)
		return
	}

	nextState := handshake[len(handshake)-1]

	if nextState == 1 {
		// assume the packet is 0x00
		_, err := readPacket(conn)

		if err != nil {
			log.Println("error reading status request:", err)
			return
		}

		var buffer bytes.Buffer

		writeByte(&buffer, 0x00)

		playersLock.RLock()
		online := len(players)
		playersLock.RUnlock()

		data := map[string]any{
			"version": map[string]any{
				"name":     "1.8.8",
				"protocol": 47,
			},
			"players": map[string]any{
				"online": online,
				"max":    69,
			},
			"description": map[string]string{
				"text":  "sugma",
				"color": "aqua",
			},
		}

		serialized, _ := json.Marshal(data)

		writeString(&buffer, string(serialized))

		if err := writePacket(conn, buffer.Bytes()); err != nil {
			log.Println("error responding to status request:", err)
			return
		}

		ping, err := readPacket(conn)

		if err != nil {
			log.Println("error reading ping packet:", err)
			return
		}

		if err := writePacket(conn, ping); err != nil {
			log.Println("error pinging client back:", err)
			return
		}

		return
	}

	loginStart, err := readPacketReader(conn)

	if err != nil {
		log.Println("error reading login start:", err)
		return
	}

	readByte(loginStart)

	username, err := readString(loginStart)

	if err != nil {
		log.Println("error reading login start username:", err)
		return
	}

	if !checkUsername(username) {
		log.Println("invalid username")
		return
	}

	playersLock.Lock()

	player.name = username
	player.conn = conn
	player.queue = make(chan []byte)

	if _, ok := players[username]; ok {
		log.Println("username in use")
		playersLock.Unlock()
		return
	}

	players[username] = &player

	playersLock.Unlock()

	var bundle bytes.Buffer

	writeLoginSuccess(&bundle, "d7bb14b6-bfe9-462f-bb00-85b91826381a", username)
	writeJoinGame(&bundle, 123, 1, 0, 0, 69, "flat", false)
	writePositionLook(&bundle, 0, 64, 0, 0, 0, 0)

	if _, err := bundle.WriteTo(conn); err != nil {
		log.Println("error sending join packets:", err)
		return
	}

	// TESTING

	/*writeEmptyChunk(conn, 0, 0)
	writeEmptyChunk(conn, 1, 0)
	writeEmptyChunk(conn, 0, 1)
	writeEmptyChunk(conn, 1, 1)

	for x := 0; x < 32; x++ {
		for z := 0; z < 32; z++ {
			writeBlockChange(conn, x, 63, z, 80, 0)
		}
	}*/

	var chunk bytes.Buffer

	// blocks (n * 8192)
	// blocklight (n * 2048)
	// skylight (n * 2048)
	// biomes (256)

	n := 16
	total := int32(n*8192 + n*2048 + n*2048 + 256)

	writeByte(&chunk, 0x21)
	writeInt(&chunk, 0)
	writeInt(&chunk, 0)
	writeBool(&chunk, true)
	writeUnsignedShort(&chunk, 0b1111111111111111)
	writeVarInt(&chunk, total)

	data := make([]byte, total)

	y := 63

	for x := 0; x < 16; x++ {
		for z := 0; z < 16; z++ {
			i := y << 8 | z << 4 | x
			t := 35
			d := (x + z)
			data[2*i] = byte((t << 4) | d)
			data[2*i+1] = byte(t >> 4)
		}
	}

	for i := 0; i < n*2048; i++ {
		data[n*8192 + i] = 0xFF
	}

	for i := 0; i < n*2048; i++ {
		data[n*8192 + n*2048 + i] = 0xFF
	}

	write(&chunk, data)

	writePacket(conn, chunk.Bytes())

	// TESTING


	go func() {
		for packet := range player.queue {
			player.conn.Write(packet)
		}
	}()

	var joinMessage bytes.Buffer
	writeChatMessageRaw(&joinMessage, "\u00a78[\u00a7b+\u00a78] \u00a77"+username)
	broadcastPacket(joinMessage.Bytes())

	for {
		packet, err := readPacketReader(conn)

		if err != nil {
			log.Println("error reading packet", err)
			return
		}

		id, _ := readByte(packet)

		switch id {
		case 0x01:
			message, err := readString(packet)

			if err != nil {
				log.Println("error reading message:", err)
				return
			}

			if len(message) > 100 {
				log.Println("message too long:", len(message))
				return
			}

			var broadcasted bytes.Buffer
			writeChatMessage(&broadcasted, player.name, message)
			broadcastPacket(broadcasted.Bytes())
		case 0x03:
			break
		case 0x04:
			if packet.Len() != 25 {
				log.Println("not enough bytes for player position")
				return
			}

			x, _ := readDouble(packet)
			y, _ := readDouble(packet)
			z, _ := readDouble(packet)
			grounded, _ := readBool(packet)

			player.x = x
			player.y = y
			player.z = z
			player.grounded = grounded
		case 0x05:
			if packet.Len() != 9 {
				log.Println("not enough bytes for player look")
			}

			yaw, _ := readFloat(packet)
			pitch, _ := readFloat(packet)
			grounded, _ := readBool(packet)

			player.yaw = yaw
			player.pitch = pitch
			player.grounded = grounded
		case 0x06:
			if packet.Len() != 33 {
				log.Println("not enough bytes for player position and look")
				return
			}

			x, _ := readDouble(packet)
			y, _ := readDouble(packet)
			z, _ := readDouble(packet)
			yaw, _ := readFloat(packet)
			pitch, _ := readFloat(packet)
			grounded, _ := readBool(packet)

			player.x = x
			player.y = y
			player.z = z
			player.yaw = yaw
			player.pitch = pitch
			player.grounded = grounded
		default:
			log.Println("ignored", packet)
		}
	}
}

func main() {
	listener, err := net.Listen("tcp", ":6969")

	if err != nil {
		panic(err)
	}

	log.Println("Listening on", listener.Addr())

	for {
		conn, _ := listener.Accept()
		go handleConnection(conn)
	}
}
