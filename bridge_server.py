from __future__ import annotations

import json
import threading
import time
from pathlib import Path
from typing import Any

from fastapi import FastAPI, HTTPException, Query
from fastapi.middleware.cors import CORSMiddleware
from pydantic import BaseModel, Field


class Node(BaseModel):
    id: str
    x: float
    y: float
    port: int


class Edge(BaseModel):
    a: str
    b: str
    cost: int = Field(gt=0)


class TopologyPayload(BaseModel):
    nodes: list[Node]
    edges: list[Edge]
    source: str = "ui"


class EventPayload(BaseModel):
    type: str
    msg: str
    source: str = "ui"


class NodeDeletePayload(BaseModel):
    id: str


class ConnectPayload(BaseModel):
    a: str
    b: str
    cost: int = Field(gt=0)


class DisconnectPayload(BaseModel):
    a: str
    b: str


class CostPayload(BaseModel):
    a: str
    b: str
    cost: int = Field(gt=0)


class BridgeState:
    def __init__(self) -> None:
        self.nodes: list[dict[str, Any]] = []
        self.edges: list[dict[str, Any]] = []
        self.events: list[dict[str, Any]] = []
        self.revision = 0
        self.event_id = 0
        self.lock = threading.Lock()

    def snapshot(self) -> dict[str, Any]:
        with self.lock:
            return {
                "nodes": self.nodes,
                "edges": self.edges,
                "revision": self.revision,
                "latestEventId": self.event_id,
            }

    def add_event(self, event_type: str, msg: str, source: str = "bridge") -> dict[str, Any]:
        with self.lock:
            self.event_id += 1
            event = {
                "id": self.event_id,
                "time": time.strftime("%H:%M:%S"),
                "type": event_type,
                "msg": msg,
                "source": source,
            }
            self.events.append(event)
            if len(self.events) > 5000:
                self.events = self.events[-2000:]
            return event

    def set_topology(self, nodes: list[dict[str, Any]], edges: list[dict[str, Any]], source: str) -> None:
        with self.lock:
            self.nodes = nodes
            self.edges = edges
            self.revision += 1
        self.add_event("recompute", f"Topology updated by {source}; revision {self.revision}", source)


STATE = BridgeState()
LOG_DIR = Path("logs")
LOG_GLOB = "*.jsonl"


def normalize_edge(a: str, b: str) -> tuple[str, str]:
    return (a, b) if a <= b else (b, a)


def edge_exists(edges: list[dict[str, Any]], a: str, b: str) -> bool:
    aa, bb = normalize_edge(a, b)
    for edge in edges:
        x, y = normalize_edge(edge["a"], edge["b"])
        if (x, y) == (aa, bb):
            return True
    return False


app = FastAPI(title="TCP Bridge API", version="1.0.0")
app.add_middleware(
    CORSMiddleware,
    allow_origins=["*"],
    allow_credentials=False,
    allow_methods=["*"],
    allow_headers=["*"],
)


@app.get("/health")
def health() -> dict[str, Any]:
    return {"ok": True, "logDir": str(LOG_DIR.resolve())}


@app.get("/state")
def state() -> dict[str, Any]:
    return STATE.snapshot()


@app.get("/events")
def events(since: int = Query(default=0, ge=0)) -> dict[str, Any]:
    with STATE.lock:
        ev = [e for e in STATE.events if e["id"] > since]
        return {
            "events": ev,
            "latestEventId": STATE.event_id,
            "revision": STATE.revision,
            "nodes": STATE.nodes,
            "edges": STATE.edges,
        }


@app.post("/topology")
def set_topology(payload: TopologyPayload) -> dict[str, Any]:
    STATE.set_topology(
        [n.model_dump() for n in payload.nodes],
        [e.model_dump() for e in payload.edges],
        payload.source,
    )
    return {"ok": True, "revision": STATE.revision}


@app.post("/event")
def add_event(payload: EventPayload) -> dict[str, Any]:
    event = STATE.add_event(payload.type, payload.msg, payload.source)
    return {"ok": True, "event": event}


@app.post("/node/add")
def add_node(payload: Node) -> dict[str, Any]:
    with STATE.lock:
        if any(n["id"] == payload.id for n in STATE.nodes):
            raise HTTPException(status_code=409, detail=f"Node {payload.id} already exists")
        STATE.nodes.append(payload.model_dump())
        STATE.revision += 1
    STATE.add_event("add", f"Node {payload.id} added via API", "ui")
    return {"ok": True, "revision": STATE.revision}


@app.post("/node/delete")
def delete_node(payload: NodeDeletePayload) -> dict[str, Any]:
    with STATE.lock:
        before_nodes = len(STATE.nodes)
        STATE.nodes = [n for n in STATE.nodes if n["id"] != payload.id]
        STATE.edges = [e for e in STATE.edges if e["a"] != payload.id and e["b"] != payload.id]
        if len(STATE.nodes) == before_nodes:
            raise HTTPException(status_code=404, detail=f"Node {payload.id} not found")
        STATE.revision += 1
    STATE.add_event("delete", f"Node {payload.id} removed via API", "ui")
    return {"ok": True, "revision": STATE.revision}


@app.post("/connect")
def connect(payload: ConnectPayload) -> dict[str, Any]:
    with STATE.lock:
        node_ids = {n["id"] for n in STATE.nodes}
        if payload.a not in node_ids or payload.b not in node_ids:
            raise HTTPException(status_code=404, detail="Both nodes must exist before connecting")
        if edge_exists(STATE.edges, payload.a, payload.b):
            raise HTTPException(status_code=409, detail="Edge already exists")
        STATE.edges.append(payload.model_dump())
        STATE.revision += 1
    STATE.add_event("connect", f"Edge {payload.a} <-> {payload.b} cost={payload.cost} via API", "ui")
    return {"ok": True, "revision": STATE.revision}


@app.post("/disconnect")
def disconnect(payload: DisconnectPayload) -> dict[str, Any]:
    with STATE.lock:
        before = len(STATE.edges)
        aa, bb = normalize_edge(payload.a, payload.b)
        STATE.edges = [
            e
            for e in STATE.edges
            if normalize_edge(e["a"], e["b"]) != (aa, bb)
        ]
        if len(STATE.edges) == before:
            raise HTTPException(status_code=404, detail="Edge not found")
        STATE.revision += 1
    STATE.add_event("delete", f"Edge {payload.a} <-> {payload.b} removed via API", "ui")
    return {"ok": True, "revision": STATE.revision}


@app.post("/link/cost")
def update_cost(payload: CostPayload) -> dict[str, Any]:
    with STATE.lock:
        aa, bb = normalize_edge(payload.a, payload.b)
        changed = False
        for edge in STATE.edges:
            if normalize_edge(edge["a"], edge["b"]) == (aa, bb):
                edge["cost"] = payload.cost
                changed = True
                break
        if not changed:
            raise HTTPException(status_code=404, detail="Edge not found")
        STATE.revision += 1
    STATE.add_event("update", f"Edge {payload.a} <-> {payload.b} cost={payload.cost} via API", "ui")
    return {"ok": True, "revision": STATE.revision}


@app.post("/reset")
def reset() -> dict[str, Any]:
    with STATE.lock:
        STATE.nodes = []
        STATE.edges = []
        STATE.revision += 1
    STATE.add_event("system", "Topology reset via API", "ui")
    return {"ok": True, "revision": STATE.revision}


def read_json_lines(path: Path, offset: int) -> tuple[list[dict[str, Any]], int]:
    parsed: list[dict[str, Any]] = []
    if not path.exists():
        return parsed, offset

    with path.open("r", encoding="utf-8") as handle:
        handle.seek(offset)
        for raw in handle:
            line = raw.strip()
            if not line:
                continue
            try:
                item = json.loads(line)
            except json.JSONDecodeError:
                continue
            if isinstance(item, dict):
                parsed.append(item)
        new_offset = handle.tell()
    return parsed, new_offset


def tail_logs_worker() -> None:
    offsets: dict[Path, int] = {}
    LOG_DIR.mkdir(parents=True, exist_ok=True)
    while True:
        for path in LOG_DIR.glob(LOG_GLOB):
            if path not in offsets:
                offsets[path] = 0
            entries, offsets[path] = read_json_lines(path, offsets[path])
            for entry in entries:
                event_type = str(entry.get("event", "system"))
                node = entry.get("node")
                target = entry.get("target")
                cwnd = entry.get("cwnd")
                msg_parts = []
                if node is not None:
                    msg_parts.append(f"node={node}")
                if target is not None:
                    msg_parts.append(f"target={target}")
                if cwnd is not None:
                    msg_parts.append(f"cwnd={cwnd}")
                if not msg_parts:
                    msg_parts.append(json.dumps(entry, ensure_ascii=True))
                STATE.add_event(event_type, " | ".join(msg_parts), "c-log")
        time.sleep(0.5)


@app.on_event("startup")
def on_startup() -> None:
    watcher = threading.Thread(target=tail_logs_worker, daemon=True)
    watcher.start()
    STATE.add_event("system", "Bridge server started", "bridge")


if __name__ == "__main__":
    import uvicorn

    uvicorn.run("bridge_server:app", host="0.0.0.0", port=8080, reload=False)
