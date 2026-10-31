"""Export Fudan native 4DGS checkpoints to Orbelio's conditional interchange format.

Format source: LogosIntelligenceLab/4d-gaussian-splatting, commit
121e360ce23d6e9ca7734072393414c844dfaea2, GaussianModel.capture().
No upstream runtime, CUDA source or pickle code is imported by the viewer.
"""
import argparse
from datetime import datetime, timezone
import hashlib
import json
import math
import os
from pathlib import Path
import struct
import tempfile

import numpy as np

HEADER = struct.Struct("<8sIIQIIII3d")
CHUNK_SIZE = 16384
MAX_SAMPLES = 1_000_000
PARAMETER_LIMIT = 1e10


def require(condition, message):
    if not condition:
        raise ValueError(message)


def tensor_array(value, shape, name):
    import torch
    require(isinstance(value, torch.Tensor) and tuple(value.shape) == shape, f"Invalid {name} tensor shape")
    result = value.detach().cpu().numpy().astype(np.float64)
    require(np.isfinite(result).all() and (np.abs(result) <= PARAMETER_LIMIT).all(), f"Invalid {name} values")
    return result


def normalize(values):
    norms = np.linalg.norm(values, axis=-1, keepdims=True)
    require((norms > 1e-12).all(), "Zero Gaussian quaternion")
    return values / norms


def rotation4(left, right):
    a, b, c, d = normalize(left).T
    p, q, r, s = normalize(right).T
    left_matrix = np.stack((a, -b, -c, -d, b, a, -d, c, c, d, a, -b, d, -c, b, a), -1).reshape(-1, 4, 4)
    right_matrix = np.stack((p, q, r, s, -q, p, -s, r, -r, s, p, -q, -s, -r, q, p), -1).reshape(-1, 4, 4)
    return (left_matrix @ right_matrix)[:, ::-1, ::-1]


def matrix_quaternions(matrices):
    result = np.empty((len(matrices), 4), dtype=np.float64)
    for index, matrix in enumerate(matrices):
        # Choosing the largest quaternion component avoids cancellation near a half turn.
        squared = np.array((1 + np.trace(matrix), 1 + 2 * matrix[0, 0] - np.trace(matrix),
                            1 + 2 * matrix[1, 1] - np.trace(matrix), 1 + 2 * matrix[2, 2] - np.trace(matrix)))
        pivot = int(np.argmax(squared))
        if pivot == 0:
            value = (squared[0], matrix[2, 1] - matrix[1, 2], matrix[0, 2] - matrix[2, 0], matrix[1, 0] - matrix[0, 1])
        elif pivot == 1:
            value = (matrix[2, 1] - matrix[1, 2], squared[1], matrix[0, 1] + matrix[1, 0], matrix[0, 2] + matrix[2, 0])
        elif pivot == 2:
            value = (matrix[0, 2] - matrix[2, 0], matrix[0, 1] + matrix[1, 0], squared[2], matrix[1, 2] + matrix[2, 1])
        else:
            value = (matrix[1, 0] - matrix[0, 1], matrix[0, 2] + matrix[2, 0], matrix[1, 2] + matrix[2, 1], squared[3])
        result[index] = value
    return normalize(result)


def condition(scales, left, right):
    rotation = rotation4(left, right)
    covariance = (rotation * np.exp(2 * scales)[:, None, :]) @ rotation.transpose(0, 2, 1)
    variance = covariance[:, 3, 3]
    cross = covariance[:, :3, 3]
    spatial = covariance[:, :3, :3] - cross[:, :, None] * cross[:, None, :] / variance[:, None, None]
    eigenvalues, axes = np.linalg.eigh(spatial)
    require((eigenvalues > 0).all() and np.isfinite(eigenvalues).all(), "Degenerate conditional covariance")
    axes[:, :, 0] *= np.where(np.linalg.det(axes) < 0, -1, 1)[:, None]
    return .5 * np.log(eigenvalues), matrix_quaternions(axes), cross / variance[:, None], variance[:, None]


class FduCheckpointConverter:
    def __init__(self, checkpoint, force_sh_3d):
        import torch
        with torch.serialization.safe_globals([np._core.multiarray.scalar, np.dtype,
                                               np.dtypes.Float32DType, np.dtypes.Float64DType]):
            saved = torch.load(checkpoint, map_location="cpu", weights_only=True)
        require(isinstance(saved, (tuple, list)) and len(saved) == 2, "Expected (GaussianModel.capture(), iteration)")
        state, self.iteration = saved
        require(isinstance(state, (tuple, list)) and len(state) == 19, "Expected official 19-field native 4D checkpoint")
        require(isinstance(state[0], int) and 0 <= state[0] <= 3, "Unsupported active spatial SH degree")
        require(isinstance(state[18], int) and 0 <= state[18] <= 2, "Unsupported active temporal SH degree")
        require(isinstance(state[16], bool), "Invalid rot_4d flag")
        require(state[17] is None or (isinstance(state[17], torch.Tensor) and state[17].numel() == 0),
                "Environment maps are not supported")
        require(not force_sh_3d or state[18] == 0, "force_sh_3d conflicts with active temporal SH")
        self.state = state
        self.degree = state[0]
        self.temporal_degree = 0 if force_sh_3d else state[18]
        require(not self.temporal_degree or self.degree == 3, "Temporal SH requires active spatial degree 3")
        self.count = len(state[1])
        require(0 < self.count <= 8_000_000, "Invalid Gaussian count")
        self.rotated = state[16]
        self.coefficients = (self.degree + 1) ** 2
        self.channels = self.coefficients * (self.temporal_degree + 1)
        require(isinstance(state[3], torch.Tensor) and state[3].ndim == 3 and state[3].shape[0] == self.count
                and state[3].shape[2] == 3 and state[3].shape[1] + 1 >= self.channels, "Invalid SH coefficient layout")
        if not force_sh_3d and not self.temporal_degree:
            require(state[3].shape[1] + 1 == self.coefficients, "Ambiguous non-Fourier 4D SH; use --force-sh-3d only for force_sh_3d training")

    def chunk(self, first, last):
        state = self.state
        size = last - first
        def read(index, suffix, name):
            require(tuple(state[index].shape) == (self.count,) + suffix, f"Invalid {name} shape")
            return tensor_array(state[index][first:last], (size,) + suffix, name)
        xyz = read(1, (3,), "xyz")
        dc = read(2, (1, 3), "SH DC")
        rest = tensor_array(state[3][first:last], (size, state[3].shape[1], 3), "SH rest")
        scales = read(4, (3,), "log scale")
        left = normalize(read(5, (4,), "rotation"))
        opacity = read(6, (1,), "opacity logit")
        times = read(13, (1,), "time")
        scale_t = read(14, (1,), "log temporal scale")
        require((np.abs(scales) <= 40).all() and (np.abs(scale_t) <= 40).all(), "Scale out of range")
        if self.rotated:
            scales, left, velocity, variance = condition(np.concatenate((scales, scale_t), 1), left, read(15, (4,), "right rotation"))
        else:
            velocity = np.zeros_like(xyz)
            variance = np.exp(2 * scale_t)
        sh = np.concatenate((dc, rest), 1)[:, :self.channels].reshape(size, -1)
        rows = np.concatenate((xyz, times, scales, left, velocity, variance, opacity, sh), 1)
        require(np.isfinite(rows).all() and (np.abs(rows) <= PARAMETER_LIMIT).all(), "Converted parameter out of range")
        require((np.abs(scales) <= 40).all(), "Conditional scale out of range")
        return rows.astype("<f4").tobytes()


def convert(checkpoint, output, time_range, sample_rate=30., force_sh_3d=False):
    start, end = time_range
    require(all(math.isfinite(v) for v in (start, end, sample_rate)) and end > start and sample_rate > 0
            and (end - start) * sample_rate <= MAX_SAMPLES, "Invalid time range or sample rate")
    require(checkpoint.resolve() != output.resolve(), "Output must differ from checkpoint")
    require(not output.exists(), "Output exists; choose a new destination")
    model = FduCheckpointConverter(checkpoint, force_sh_3d)
    output.parent.mkdir(parents=True, exist_ok=True)
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(dir=output.parent, delete=False, suffix=".tmp") as stream:
            temporary = Path(stream.name)
            stream.write(HEADER.pack(b"FDU4DGS\0", 1, HEADER.size, model.count, model.degree,
                                     model.temporal_degree, 0, 0, start, end, sample_rate))
            for first in range(0, model.count, CHUNK_SIZE):
                stream.write(model.chunk(first, min(model.count, first + CHUNK_SIZE)))
            stream.flush()
            os.fsync(stream.fileno())
        temporary.rename(output)
    finally:
        if temporary and temporary.exists():
            temporary.unlink()
    return {"gaussians": model.count, "spatialDegree": model.degree, "temporalDegree": model.temporal_degree,
            "rot4d": model.rotated, "iteration": model.iteration, "timeRange": time_range,
            "sampleRate": sample_rate, "output": str(output.resolve()), "complete": True}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("checkpoint", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--time-range", nargs=2, type=float, required=True, metavar=("START", "END"))
    parser.add_argument("--sample-rate", type=float, default=30.)
    parser.add_argument("--force-sh-3d", action="store_true")
    parser.add_argument("--report", type=Path)
    args = parser.parse_args()
    stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S%fZ")
    report_path = args.report or args.output.with_suffix(f".conversion-{stamp}.json")
    report = {"complete": False, "checkpoint": str(args.checkpoint.resolve())}
    try:
        require(report_path.resolve() not in (args.checkpoint.resolve(), args.output.resolve()),
                "Report must differ from checkpoint and output")
        with args.checkpoint.open("rb") as source:
            report["checkpointSha256"] = hashlib.file_digest(source, "sha256").hexdigest()
        report.update(convert(args.checkpoint, args.output, args.time_range, args.sample_rate, args.force_sh_3d))
        return 0
    except Exception as error:
        report["error"] = str(error)
        print(str(error))
        return 1
    finally:
        if report_path.resolve() in (args.checkpoint.resolve(), args.output.resolve()):
            report_path = args.output.with_suffix(f".conversion-error-{stamp}.json")
        report_path.parent.mkdir(parents=True, exist_ok=True)
        report_path.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    raise SystemExit(main())
