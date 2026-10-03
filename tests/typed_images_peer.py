#!/usr/bin/env python3
"""Run the isolated ImagesClient behavior binary against a private HTTPS peer."""
import base64
import http.server
import json
import os
import ssl
import subprocess
import sys
import tempfile
import threading
import time

PNG = base64.b64decode("iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+aK1sAAAAASUVORK5CYII=")


class Peer(http.server.BaseHTTPRequestHandler):
    count = 0
    lock = threading.Lock()

    def log_message(self, *args):
        pass

    def reply(self, status, value):
        body = json.dumps(value).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Connection", "close")
        self.end_headers()
        try:
            self.wfile.write(body)
        except (BrokenPipeError, ConnectionResetError, ssl.SSLError):
            pass

    def do_GET(self):
        with self.lock:
            count = self.count
        self.reply(200, {"requests": count})

    def do_POST(self):
        with self.lock:
            type(self).count += 1
        length = int(self.headers["Content-Length"])
        request = json.loads(self.rfile.read(length))
        google = self.path.endswith(":generateContent")
        prompt = request["contents"][0]["parts"][0]["text"] if google else request["prompt"]
        if prompt == "slow":
            time.sleep(0.5)
        if prompt == "provider_error":
            self.reply(429, {"error": {"message": "credential-private body-private https://private/?token=secret", "code": "private"}})
            return
        if prompt == "redirect":
            self.send_response(307)
            self.send_header("Location", "https://localhost/private?token=secret")
            self.send_header("Content-Length", "0")
            self.end_headers()
            return
        if prompt == "malformed":
            self.reply(200, {"candidates": "wrong", "data": "wrong"})
            return
        encoded = base64.b64encode(PNG).decode()
        if prompt == "bad_base64":
            encoded = "iVBORw0KGgo=AA=="
        if prompt == "truncated_png":
            encoded = base64.b64encode(PNG[:-1]).decode()
        if prompt == "noncanonical_base64":
            encoded = encoded[:-2] + "J="  # Nonzero unused low bits.
        if google:
            image = {"inlineData": {"mimeType": "image/jpeg" if prompt == "bad_mime" else "image/png", "data": encoded}}
            parts = [{"text": "before"}, image, {"text": "after"}]
            response = {"responseId": "fixture-image-1", "modelVersion": "gemini-3.1-flash-lite-image", "candidates": [{"finishReason": "STOP", "content": {"parts": parts}}]}
            if prompt == "blocked":
                response = {"promptFeedback": {"blockReason": "SAFETY"}}
            if prompt == "no_image":
                response["candidates"][0]["content"]["parts"] = [{"text": "no image"}]
            if prompt == "usage":
                response["usageMetadata"] = {"promptTokenCount": 9, "totalTokenCount": 1129, "imageNovelMetric": {"unknown": 7}}
            if prompt == "bad_usage":
                response["usageMetadata"] = {"promptTokenCount": -1}
        else:
            data = []
            for index in range(request["n"]):
                artifact = {"revised_prompt": "revision-" + str(index)}
                if request.get("response_format") == "url":
                    artifact["url"] = "https://images.example/image-" + str(index) + "?token=owned-private"
                else:
                    artifact["b64_json"] = encoded
                data.append(artifact)
            response = {"created": 42, "data": data}
            if prompt == "bad_mime":
                response["output_format"] = "jpeg"
            if prompt == "usage":
                response["usage"] = {"input_tokens": 7, "unknown_tokens": 11}
        self.reply(200, response)


def main():
    if len(sys.argv) < 2:
        raise SystemExit("usage: typed_images_peer.py <gtest-binary> [gtest options]")
    with tempfile.TemporaryDirectory(prefix="neograph-images-") as directory:
        cert = os.path.join(directory, "cert.pem")
        key = os.path.join(directory, "key.pem")
        subprocess.run(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-keyout", key, "-out", cert, "-days", "1", "-subj", "/CN=localhost", "-addext", "subjectAltName=DNS:localhost,IP:127.0.0.1", "-addext", "basicConstraints=critical,CA:TRUE"], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Peer)
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(cert, key)
        server.socket = context.wrap_socket(server.socket, server_side=True)
        thread = threading.Thread(target=server.serve_forever)
        thread.start()
        environment = os.environ.copy()
        environment["SSL_CERT_FILE"] = cert
        environment["NEOGRAPH_IMAGES_TEST_PORT"] = str(server.server_port)
        try:
            result = subprocess.run(sys.argv[1:], env=environment, check=False)
        finally:
            server.shutdown()
            thread.join()
            server.server_close()
        return result.returncode


if __name__ == "__main__":
    sys.exit(main())
