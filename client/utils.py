import socket

def recvall(sock, n):
    """【网络通信】从 Socket 接收完整的 n 字节数据，解决半包/粘包"""
    data = bytearray()
    while len(data) < n:
        packet = sock.recv(n - len(data))
        if not packet:
            return None
        data.extend(packet)
    return data