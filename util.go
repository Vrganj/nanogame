package main

func checkUsername(username string) bool {
	if len(username) < 1 || len(username) > 16 {
		return false
	}

	for _, c := range username {
		if !((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_') {
			return false
		}
	}

	return true
}