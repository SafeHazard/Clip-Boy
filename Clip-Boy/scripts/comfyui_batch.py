"""Batch-submit collectible image prompts to ComfyUI via its API.

Reads the manifest.csv from gen_image_prompts.py, loads a ComfyUI workflow
template, swaps in each prompt, and submits. Polls for completion and saves
the output PNGs.

Prerequisites:
  - ComfyUI running (default: http://127.0.0.1:8188)
  - A workflow JSON exported from ComfyUI (API format, not UI format)
    - In ComfyUI: Settings > Enable Dev Mode, then Save (API Format)
  - Flux.1 Dev model loaded in ComfyUI

Usage:
  py -3 scripts/comfyui_batch.py --workflow workflow_api.json --prompts prompts/ --out generated/
  py -3 scripts/comfyui_batch.py --workflow workflow_api.json --prompts prompts/ --out generated/ --format flux
  py -3 scripts/comfyui_batch.py --workflow workflow_api.json --prompts prompts/ --out generated/ --ids 2,3,40

The workflow JSON must contain a node with class_type "CLIPTextEncode" (or
similar) whose "text" input will be replaced with each prompt. The script
auto-detects the positive prompt node. For SDXL workflows with a negative
prompt node, it sets that too.

Requires: pip install websocket-client requests
"""

import argparse
import csv
import json
import os
import sys
import time
import uuid
import urllib.request
import urllib.parse

# ─────────────────────── ComfyUI API helpers ──────────────────────────────────

def queue_prompt(workflow, server="127.0.0.1:8188", client_id=None):
    """Submit a workflow to ComfyUI's /prompt endpoint. Returns prompt_id."""
    if client_id is None:
        client_id = str(uuid.uuid4())
    payload = json.dumps({
        "prompt": workflow,
        "client_id": client_id,
    }).encode("utf-8")
    req = urllib.request.Request(
        f"http://{server}/prompt",
        data=payload,
        headers={"Content-Type": "application/json"},
    )
    with urllib.request.urlopen(req) as resp:
        result = json.loads(resp.read())
    return result.get("prompt_id"), client_id


def get_history(prompt_id, server="127.0.0.1:8188"):
    """Poll /history/{prompt_id} until the job completes."""
    url = f"http://{server}/history/{prompt_id}"
    with urllib.request.urlopen(url) as resp:
        return json.loads(resp.read())


def get_image(filename, subfolder, folder_type, server="127.0.0.1:8188"):
    """Download a generated image from ComfyUI's /view endpoint."""
    params = urllib.parse.urlencode({
        "filename": filename,
        "subfolder": subfolder,
        "type": folder_type,
    })
    url = f"http://{server}/view?{params}"
    with urllib.request.urlopen(url) as resp:
        return resp.read()


def wait_for_completion(prompt_id, server="127.0.0.1:8188", timeout=300):
    """Poll until prompt_id appears in history with outputs."""
    start = time.time()
    while time.time() - start < timeout:
        history = get_history(prompt_id, server)
        if prompt_id in history:
            status = history[prompt_id].get("status", {})
            if status.get("completed", False) or "outputs" in history[prompt_id]:
                return history[prompt_id]
            # Check for error
            if status.get("status_str") == "error":
                msgs = status.get("messages", [])
                raise RuntimeError(f"ComfyUI error: {msgs}")
        time.sleep(2)
    raise TimeoutError(f"Timed out waiting for prompt {prompt_id} after {timeout}s")


# ─────────────────────── Workflow manipulation ────────────────────────────────

def find_prompt_nodes(workflow):
    """Find the positive (and optionally negative) CLIP text encode nodes.

    Returns (positive_node_id, negative_node_id_or_None).
    Heuristic: looks for CLIPTextEncode nodes. If two exist, the one connected
    to a KSampler's "positive" input is positive, the other is negative.
    """
    clip_nodes = []
    for node_id, node in workflow.items():
        ct = node.get("class_type", "")
        if "CLIPTextEncode" in ct:
            clip_nodes.append(node_id)

    if len(clip_nodes) == 0:
        # Try FluxGuidance or similar
        for node_id, node in workflow.items():
            ct = node.get("class_type", "")
            if "FluxGuidance" in ct:
                clip_nodes.append(node_id)

    if len(clip_nodes) == 0:
        raise ValueError("No CLIPTextEncode or FluxGuidance node found in workflow")

    if len(clip_nodes) == 1:
        return clip_nodes[0], None

    # Try to determine which is positive vs negative by tracing KSampler inputs
    for node_id, node in workflow.items():
        ct = node.get("class_type", "")
        if "KSampler" in ct or "Sampler" in ct:
            inputs = node.get("inputs", {})
            pos_ref = inputs.get("positive")
            neg_ref = inputs.get("negative")
            pos_id = pos_ref[0] if isinstance(pos_ref, list) else None
            neg_id = neg_ref[0] if isinstance(neg_ref, list) else None
            if pos_id in clip_nodes:
                neg_id = neg_id if neg_id in clip_nodes else None
                return str(pos_id), str(neg_id) if neg_id else None

    # Fallback: first is positive, second is negative
    return clip_nodes[0], clip_nodes[1] if len(clip_nodes) > 1 else None


def set_prompt_text(workflow, pos_node_id, text, neg_node_id=None, neg_text=None):
    """Set the text input on the identified CLIP nodes."""
    workflow[pos_node_id]["inputs"]["text"] = text
    if neg_node_id and neg_text:
        workflow[neg_node_id]["inputs"]["text"] = neg_text


def set_seed(workflow, seed):
    """Set seed on KSampler node(s) for reproducibility."""
    for node_id, node in workflow.items():
        ct = node.get("class_type", "")
        if "KSampler" in ct or "Sampler" in ct:
            inputs = node.get("inputs", {})
            if "seed" in inputs:
                inputs["seed"] = seed
            if "noise_seed" in inputs:
                inputs["noise_seed"] = seed


# ─────────────────────── Main ─────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser(description="Batch-submit prompts to ComfyUI")
    parser.add_argument("--workflow", required=True, help="ComfyUI API workflow JSON")
    parser.add_argument("--prompts", default="prompts", help="Directory with prompt .txt files")
    parser.add_argument("--manifest", default=None, help="Manifest CSV (default: <prompts>/manifest.csv)")
    parser.add_argument("--out", default="generated", help="Output directory for PNGs")
    parser.add_argument("--server", default="127.0.0.1:8000", help="ComfyUI server address")
    parser.add_argument("--format", choices=["flux", "sdxl"], default="flux",
                        help="Which prompt format to use from manifest")
    parser.add_argument("--ids", default=None, help="Comma-separated IDs to generate (default: all)")
    parser.add_argument("--seed", type=int, default=42, help="Base seed for reproducibility")
    parser.add_argument("--timeout", type=int, default=300, help="Timeout per image in seconds")
    parser.add_argument("--delay", type=float, default=1.0, help="Delay between submissions")
    args = parser.parse_args()

    # Load workflow
    with open(args.workflow, "r", encoding="utf-8") as f:
        base_workflow = json.load(f)

    # Find prompt nodes
    pos_node, neg_node = find_prompt_nodes(base_workflow)
    print(f"Positive prompt node: {pos_node}")
    if neg_node:
        print(f"Negative prompt node: {neg_node}")

    # Load manifest
    manifest_path = args.manifest or os.path.join(args.prompts, "manifest.csv")
    if not os.path.isfile(manifest_path):
        print(f"ERROR: Manifest not found: {manifest_path}", file=sys.stderr)
        sys.exit(1)

    items = []
    with open(manifest_path, "r", encoding="utf-8") as f:
        reader = csv.DictReader(f)
        for row in reader:
            if row["format"] == args.format:
                items.append(row)

    # Filter by IDs if specified
    if args.ids:
        id_set = set(int(x.strip()) for x in args.ids.split(","))
        items = [i for i in items if int(i["id"]) in id_set]

    print(f"Queued {len(items)} items for generation ({args.format} format)")

    os.makedirs(args.out, exist_ok=True)

    # Track results
    results = []
    failed = []

    for idx, item in enumerate(items):
        item_id = int(item["id"])
        prompt_file = os.path.join(args.prompts, item["filename"])

        if not os.path.isfile(prompt_file):
            print(f"  SKIP {item_id} — prompt file not found: {prompt_file}")
            failed.append(item_id)
            continue

        # Read prompt (and negative if SDXL)
        with open(prompt_file, "r", encoding="utf-8") as pf:
            content = pf.read()

        if "---NEGATIVE---" in content:
            parts = content.split("---NEGATIVE---")
            pos_text = parts[0].strip()
            neg_text = parts[1].strip() if len(parts) > 1 else ""
        else:
            pos_text = content.strip()
            neg_text = ""

        # Clone workflow and set prompt
        workflow = json.loads(json.dumps(base_workflow))
        set_prompt_text(workflow, pos_node, pos_text, neg_node, neg_text)
        set_seed(workflow, args.seed + item_id)  # Unique but reproducible per item

        # Submit
        print(f"  [{idx+1}/{len(items)}] ID={item_id} {item['title']}...", end="", flush=True)
        try:
            prompt_id, client_id = queue_prompt(workflow, args.server)
            result = wait_for_completion(prompt_id, args.server, args.timeout)

            # Extract output images
            outputs = result.get("outputs", {})
            saved = False
            for node_id, node_out in outputs.items():
                images = node_out.get("images", [])
                for img_info in images:
                    img_data = get_image(
                        img_info["filename"],
                        img_info.get("subfolder", ""),
                        img_info.get("type", "output"),
                        args.server,
                    )
                    out_path = os.path.join(args.out, f"{item_id}.png")
                    with open(out_path, "wb") as of:
                        of.write(img_data)
                    print(f" OK -> {out_path}")
                    results.append({"id": item_id, "path": out_path})
                    saved = True
                    break  # One image per item
                if saved:
                    break

            if not saved:
                print(" WARN: no output images found")
                failed.append(item_id)

        except Exception as e:
            print(f" FAIL: {e}")
            failed.append(item_id)

        if args.delay > 0 and idx < len(items) - 1:
            time.sleep(args.delay)

    # Summary
    print(f"\nDone: {len(results)} succeeded, {len(failed)} failed")
    if failed:
        print(f"Failed IDs: {','.join(str(x) for x in failed)}")
        print(f"Re-run with: --ids {','.join(str(x) for x in failed)}")


if __name__ == "__main__":
    main()
