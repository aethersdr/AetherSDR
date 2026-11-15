"""
Signal classifier training script for AetherSDR S-History v2.

Trains a small 2D CNN on synthetic spectrogram patches to distinguish:
  Class 0 — voice/SSB  (1.8–3.3 kHz wide, time-varying amplitude with gaps)
  Class 1 — carrier    (narrow tone < 600 Hz, or digital flat-top < 1.5 kHz)

Exports the trained model to ONNX (opset 12) for use by SignalClassifier.cpp.

Requirements:
    pip install torch numpy onnx

Usage:
    python tools/train_classifier.py [--output signal_classifier.onnx] [--epochs 40]
"""

import argparse
import math
import random
import struct

import numpy as np
import torch
import torch.nn as nn
import torch.optim as optim

# ── Patch dimensions (must match SpectrogramBuffer constants) ────────────────
TIME_FRAMES  = 32
FREQ_BINS    = 64
NOISE_DB     = -120.0   # synthetic noise floor
SIGNAL_DB    = -80.0    # rough signal level above floor


# ── Synthetic data generation ────────────────────────────────────────────────

def _gaussian_blob(bins: int, center_frac: float, width_frac: float) -> np.ndarray:
    """1-D Gaussian shape spanning [0,1] freq axis."""
    x = np.linspace(0.0, 1.0, bins)
    sigma = width_frac / 3.0
    return np.exp(-0.5 * ((x - center_frac) / sigma) ** 2)


def _voice_patch(rng: random.Random, time: int, freq: int) -> np.ndarray:
    """
    Synthetic SSB/voice: 1.8–3.3 kHz width (14–26% of a 12 kHz pan),
    time-varying amplitude with speech-like envelope + inter-word gaps.
    """
    patch = np.full((time, freq), NOISE_DB, dtype=np.float32)

    # Random width 14–26 % of the patch freq window
    w_frac = rng.uniform(0.14, 0.26)
    c_frac = rng.uniform(w_frac / 2 + 0.05, 1.0 - w_frac / 2 - 0.05)
    freq_shape = _gaussian_blob(freq, c_frac, w_frac)

    # Speech-like amplitude envelope: semi-random bursts with 0-4 gaps
    amp = np.zeros(time, dtype=np.float32)
    gaps = sorted(rng.sample(range(2, time - 2), k=min(4, rng.randint(0, 4))))
    t = 0
    in_gap = False
    while t < time:
        if t in gaps:
            in_gap = not in_gap
        if not in_gap:
            amp[t] = rng.uniform(0.5, 1.0)
        t += 1

    signal_range = SIGNAL_DB - NOISE_DB  # dB above noise
    for ti in range(time):
        row_db = NOISE_DB + amp[ti] * signal_range * freq_shape
        # Add per-bin jitter to simulate spectral texture of speech
        jitter = np.array([rng.gauss(0.0, 2.0) for _ in range(freq)], dtype=np.float32)
        patch[ti] = row_db + jitter

    return patch


def _carrier_patch(rng: random.Random, time: int, freq: int) -> np.ndarray:
    """
    Synthetic narrow carrier or digital signal: < 600 Hz (< 5% of the patch),
    constant amplitude (CW beacon, OFDM pilot, FSK carrier).
    """
    patch = np.full((time, freq), NOISE_DB, dtype=np.float32)

    carrier_type = rng.choice(["tone", "fsk", "digital"])
    if carrier_type == "tone":
        w_frac = rng.uniform(0.01, 0.04)  # very narrow tone
    elif carrier_type == "fsk":
        w_frac = rng.uniform(0.04, 0.10)  # narrow FSK
    else:
        w_frac = rng.uniform(0.08, 0.18)  # flat digital (PSK/OFDM) < voice min

    c_frac = rng.uniform(w_frac / 2 + 0.05, 1.0 - w_frac / 2 - 0.05)

    if carrier_type == "digital":
        # Flat rectangular top — characteristic of PSK/OFDM
        freq_shape = np.zeros(freq, dtype=np.float32)
        lo = max(0, int((c_frac - w_frac / 2) * freq))
        hi = min(freq, int((c_frac + w_frac / 2) * freq))
        freq_shape[lo:hi] = 1.0
    else:
        freq_shape = _gaussian_blob(freq, c_frac, w_frac)

    signal_range = SIGNAL_DB - NOISE_DB
    amp = rng.uniform(0.7, 1.0)  # carriers are steady
    for ti in range(time):
        # Small amplitude variation (fading) but no speech gaps
        amp_t = amp + rng.gauss(0.0, 0.03)
        row_db = NOISE_DB + amp_t * signal_range * freq_shape
        jitter = np.array([rng.gauss(0.0, 0.5) for _ in range(freq)], dtype=np.float32)
        patch[ti] = row_db + jitter

    return patch


def make_dataset(n_samples: int, seed: int = 42):
    rng = random.Random(seed)
    X, y = [], []
    for _ in range(n_samples // 2):
        X.append(_voice_patch(rng, TIME_FRAMES, FREQ_BINS))
        y.append(0)  # voice
        X.append(_carrier_patch(rng, TIME_FRAMES, FREQ_BINS))
        y.append(1)  # carrier

    X = np.stack(X, axis=0)[:, np.newaxis, :, :]  # [N, 1, T, F]
    y = np.array(y, dtype=np.int64)

    # Normalize each patch to [0, 1]
    mn = X.min(axis=(2, 3), keepdims=True)
    mx = X.max(axis=(2, 3), keepdims=True)
    rng_val = np.where(mx - mn > 0.5, mx - mn, 1.0)
    X = (X - mn) / rng_val

    return X.astype(np.float32), y


# ── Model ────────────────────────────────────────────────────────────────────

class SignalCNN(nn.Module):
    """
    Small 2-D CNN for spectrogram patch classification.
    Input:  [N, 1, 32, 64]  (NCHW — 1 channel, 32 time frames, 64 freq bins)
    Output: [N, 2]           (voice probability, carrier probability)
    """
    def __init__(self):
        super().__init__()
        self.features = nn.Sequential(
            nn.Conv2d(1, 8, kernel_size=3, padding=1),
            nn.BatchNorm2d(8),
            nn.ReLU(inplace=True),
            nn.MaxPool2d(2, 2),                     # → [8, 16, 32]

            nn.Conv2d(8, 16, kernel_size=3, padding=1),
            nn.BatchNorm2d(16),
            nn.ReLU(inplace=True),
            nn.MaxPool2d(2, 2),                     # → [16, 8, 16]

            nn.AdaptiveAvgPool2d((4, 4)),            # → [16, 4, 4] = 256
        )
        self.classifier = nn.Sequential(
            nn.Flatten(),
            nn.Linear(256, 64),
            nn.ReLU(inplace=True),
            nn.Dropout(0.3),
            nn.Linear(64, 2),
        )

    def forward(self, x: torch.Tensor) -> torch.Tensor:
        return self.classifier(self.features(x))


# ── Training ─────────────────────────────────────────────────────────────────

def train(args):
    print(f"Generating {args.samples} synthetic patches …")
    X, y = make_dataset(args.samples, seed=args.seed)

    # 80/20 train/val split
    split = int(len(X) * 0.8)
    idx = np.random.RandomState(args.seed).permutation(len(X))
    train_idx, val_idx = idx[:split], idx[split:]

    X_tr = torch.from_numpy(X[train_idx])
    y_tr = torch.from_numpy(y[train_idx])
    X_va = torch.from_numpy(X[val_idx])
    y_va = torch.from_numpy(y[val_idx])

    device = torch.device("cuda" if torch.cuda.is_available() else "cpu")
    print(f"Training on {device} for {args.epochs} epochs …")

    model = SignalCNN().to(device)
    criterion = nn.CrossEntropyLoss()
    optimizer = optim.Adam(model.parameters(), lr=1e-3)
    scheduler = optim.lr_scheduler.CosineAnnealingLR(optimizer, T_max=args.epochs)

    bs = 64
    for epoch in range(1, args.epochs + 1):
        model.train()
        perm = torch.randperm(len(X_tr))
        total_loss = 0.0
        for i in range(0, len(X_tr), bs):
            idx_b = perm[i:i + bs]
            xb, yb = X_tr[idx_b].to(device), y_tr[idx_b].to(device)
            optimizer.zero_grad()
            loss = criterion(model(xb), yb)
            loss.backward()
            optimizer.step()
            total_loss += loss.item() * len(idx_b)
        scheduler.step()

        if epoch % 5 == 0 or epoch == args.epochs:
            model.eval()
            with torch.no_grad():
                logits = model(X_va.to(device))
                preds  = logits.argmax(dim=1).cpu()
                acc    = (preds == y_va).float().mean().item()
            print(f"  Epoch {epoch:3d}  loss={total_loss/len(X_tr):.4f}  val_acc={acc:.3f}")

    return model


# ── ONNX export ──────────────────────────────────────────────────────────────

def export_onnx(model: torch.nn.Module, output_path: str):
    model.eval()
    dummy = torch.zeros(1, 1, TIME_FRAMES, FREQ_BINS)
    # Wrap with softmax so output is directly [voiceProb, carrierProb]
    class WithSoftmax(nn.Module):
        def __init__(self, base):
            super().__init__()
            self.base = base
        def forward(self, x):
            return torch.softmax(self.base(x), dim=1)

    export_model = WithSoftmax(model)
    torch.onnx.export(
        export_model, dummy, output_path,
        input_names=["input"],
        output_names=["output"],
        dynamic_axes={"input": {0: "batch"}},
        opset_version=12,
    )
    print(f"Model exported → {output_path}")
    print(f"  Input:  [N, 1, {TIME_FRAMES}, {FREQ_BINS}]  (NCHW, float32, normalised [0,1])")
    print(f"  Output: [N, 2]  — [voice_prob, carrier_prob]  (softmax, sums to 1)")
    print()
    print("Place signal_classifier.onnx next to the AetherSDR executable")
    print("or in ~/.config/AetherSDR/ to enable CNN-assisted classification.")


# ── Entry point ──────────────────────────────────────────────────────────────

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Train AetherSDR signal classifier")
    parser.add_argument("--output",  default="signal_classifier.onnx",
                        help="Output ONNX model path (default: signal_classifier.onnx)")
    parser.add_argument("--epochs",  type=int, default=40,
                        help="Training epochs (default: 40)")
    parser.add_argument("--samples", type=int, default=8000,
                        help="Total training samples (half voice, half carrier; default: 8000)")
    parser.add_argument("--seed",    type=int, default=42)
    args = parser.parse_args()

    model = train(args)
    export_onnx(model, args.output)
