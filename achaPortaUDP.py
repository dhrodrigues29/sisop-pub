#!/usr/bin/env python3
import socket
import sys

def check_port(port):
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        sock.bind(("0.0.0.0", port))
        print(f"Porta {port} UDP livre")
    except OSError:
        print(f"Porta {port} UDP ocupada")
    finally:
        sock.close()

if __name__ == "__main__":
    if len(sys.argv) != 2:
        print("Uso: python3 check_udp.py <porta>")
        sys.exit(1)
    try:
        p = int(sys.argv[1])
    except ValueError:
        print("Porta inválida")
        sys.exit(1)
    if p < 1 or p > 65535:
        print("Porta fora do intervalo 1-65535")
        sys.exit(1)
    check_port(p)
