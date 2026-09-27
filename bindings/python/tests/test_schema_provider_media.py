"""Real schema provider loopback calls across the Python artifact boundary."""
import json
import threading
from contextlib import contextmanager
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

import neograph_engine as ng
from neograph_engine.llm import SchemaProvider


@contextmanager
def media_server():
    class Handler(BaseHTTPRequestHandler):
        def log_message(self, *_args):
            pass

        def do_POST(self):
            body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
            if self.path == "/v1/images/generations":
                assert body == {"model": "gpt-image-1", "prompt": "blue kite"}
                payload = {"data": [{"b64_json": "UE5H", "revised_prompt": "blue kite"}]}
            elif self.path == "/v1beta/models/veo-test:predictLongRunning":
                assert body == {"instances": [{"prompt": "blue kite"}]}
                payload = {"name": "models/veo-test/operations/123", "done": False}
            else:
                self.send_error(404)
                return
            data = json.dumps(payload).encode()
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)

        def do_GET(self):
            if self.path != "/v1beta/models/veo-test/operations/123":
                self.send_error(404)
                return
            data = json.dumps({"done": True, "response": {"generateVideoResponse": {
                "generatedSamples": [{"video": {"uri": "https://example.invalid/video",
                                                "mimeType": "video/mp4", "fileId": "file-5",
                                                "durationSeconds": 9}}]}}}).encode()
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)

    server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    thread = threading.Thread(target=server.serve_forever)
    thread.start()
    try:
        yield f"http://127.0.0.1:{server.server_port}"
    finally:
        server.shutdown()
        server.server_close()
        thread.join()


def test_generated_images_and_videos_from_python():
    with media_server() as base_url:
        params = ng.CompletionParams()
        params.prompt = "blue kite"
        params.model = "gpt-image-1"
        images = SchemaProvider(schema_path="openai_images", api_key="test-key",
                                base_url_override=base_url,
                                allow_insecure_loopback=True).complete(params)
        assert len(images.artifacts) == 1
        image = images.artifacts[0]
        assert (image.kind, image.mime_type, image.base64_data, image.metadata) == (
            "image", "image/png", "UE5H", "blue kite")

        params.model = "veo-test"
        videos = SchemaProvider(schema_path="veo", api_key="test-key",
                                base_url_override=base_url,
                                allow_insecure_loopback=True).complete(params)
        assert len(videos.artifacts) == 1
        video = videos.artifacts[0]
        assert (video.kind, video.mime_type, video.url, video.file_id) == (
            "video", "video/mp4", "https://example.invalid/video", "file-5")
        assert video.metadata["durationSeconds"] == 9
