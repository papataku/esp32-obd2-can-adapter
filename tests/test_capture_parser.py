#!/usr/bin/env python3
line = "@FRAME,123456,S,7E8,8,04410C1234000000,DATA"
parts = line.split(",")
assert parts[0] == "@FRAME"
assert parts[1] == "123456"
assert parts[2] == "S"
assert parts[3] == "7E8"
assert parts[4] == "8"
assert len(parts[5]) == 16
assert parts[6] == "DATA"
print("PASS Phase 1 text frame format")
