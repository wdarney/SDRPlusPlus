#!/usr/bin/env python3
"""Convert exact HF Whisper encoder weights to Channel Bank's whisper.cpp ABI."""
import argparse
import gc
import hashlib
import json
from pathlib import Path
import platform
import re
import shutil
import subprocess
import tempfile

VENDOR = "23ee03506a91ac3d3f0071b40e66a430eebdfa1d"


def encoder_name(ggml_name):
    if Path(ggml_name).name != ggml_name or not ggml_name.endswith('.bin'):
        raise ValueError('--ggml-name must be a .bin basename')
    return re.sub(r'-q._.$', '', ggml_name[:-4]) + '-encoder'


def rename_encoder_key(key):
    if key == 'embed_positions.weight':
        return 'positional_embedding'
    for old, new in [('layers.', 'blocks.'), ('self_attn_layer_norm.', 'attn_ln.'),
                     ('self_attn.q_proj.', 'attn.query.'), ('self_attn.k_proj.', 'attn.key.'),
                     ('self_attn.v_proj.', 'attn.value.'), ('self_attn.out_proj.', 'attn.out.'),
                     ('final_layer_norm.', 'mlp_ln.'), ('fc1.', 'mlp.0.'), ('fc2.', 'mlp.2.')]:
        key = key.replace(old, new)
    if key.startswith('layer_norm.'):
        key = key.replace('layer_norm.', 'ln_post.', 1)
    return key


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--model', required=True, help='Hugging Face model ID or local snapshot directory')
    ap.add_argument('--revision', help='HF commit/tag; remote IDs are resolved to a commit before loading')
    ap.add_argument('--ggml-name', required=True, help='Existing matching GGML model basename')
    ap.add_argument('--output-dir', type=Path,
                    default=Path.home() / 'Library/Application Support/sdrpp/channel_bank/models',
                    help='Output directory (default: %(default)s); existing outputs are never replaced')
    ap.add_argument('--max-relative-l2-error', type=float, default=0.02,
                    help='Maximum random-input Core ML/FP32 encoder error (default: 0.02); '
                         'override only with model-specific accuracy validation')
    args = ap.parse_args()
    if not 0 < args.max_relative_l2_error <= 1:
        ap.error('--max-relative-l2-error must be greater than zero and at most one')
    name = encoder_name(args.ggml_name)
    if platform.system() != 'Darwin' or platform.machine() != 'arm64':
        ap.error('Run conversion/compilation on an Apple Silicon Mac with Xcode installed')
    marker = Path(__file__).resolve().parents[1] / 'external/whisper.cpp/.vendored-from-commit'
    if marker.read_text().strip() != VENDOR:
        ap.error('Vendored whisper.cpp revision changed; re-audit the encoder ABI first')

    import numpy as np
    import torch
    import coremltools as ct
    from huggingface_hub import model_info
    from transformers import WhisperForConditionalGeneration
    from whisper.model import AudioEncoder
    from coreml_encoder_ane import AudioEncoderANE, linear_to_conv2d_map

    torch.manual_seed(0)
    torch.set_num_threads(4)
    args.output_dir.mkdir(parents=True, exist_ok=True)
    package = args.output_dir / (name + '.mlpackage')
    compiled = args.output_dir / (name + '.mlmodelc')
    manifest = args.output_dir / (name + '.json')
    for p in (package, compiled, manifest):
        if p.exists():
            ap.error(f'Output exists: {p}')
    local = Path(args.model).is_dir()
    revision = None if local else model_info(args.model, revision=args.revision).sha
    # No stock Whisper load_model call and no permissive/random weight fallback.
    hf, loading = WhisperForConditionalGeneration.from_pretrained(
        args.model, revision=revision, torch_dtype=torch.float32,
        attn_implementation='eager', output_loading_info=True,
        local_files_only=local, trust_remote_code=False)
    if loading.get('missing_keys') or loading.get('mismatched_keys') or loading.get('error_msgs'):
        raise RuntimeError(f'Incomplete HF checkpoint: {loading}')
    cfg = hf.config
    dims = (cfg.num_mel_bins, cfg.max_source_positions, cfg.d_model,
            cfg.encoder_attention_heads, cfg.encoder_layers)
    if cfg.max_source_positions != 1500:
        raise ValueError('This runtime requires a fixed 3000-frame mel input')
    if args.ggml_name == 'ggml-whisper-large-v3-atc-q5_0.bin' and dims != (128, 1500, 1280, 20, 32):
        raise ValueError(f'ATC Large encoder dimensions do not match large-v3: {dims}')
    source = hf.model.encoder.eval()
    # Only encoder weights are converted. Release the decoder before allocating
    # the reference encoder, especially for full large-v3 checkpoints.
    del hf
    gc.collect()
    weights = source.state_dict()
    digest = hashlib.sha256()
    mapped = {}
    for key in sorted(weights):
        value = weights[key].detach().cpu().contiguous()
        digest.update(key.encode())
        digest.update(str(tuple(value.shape)).encode())
        digest.update(value.numpy().tobytes())
        new_key = rename_encoder_key(key)
        if new_key in mapped:
            raise ValueError(f'Duplicate mapped key: {new_key}')
        mapped[new_key] = value
    reference = AudioEncoder(*dims).eval()
    reference.load_state_dict(mapped, strict=True)
    sample = torch.randn(1, cfg.num_mel_bins, 3000)
    with torch.no_grad():
        hf_result = source(sample).last_hidden_state
        reference_result = reference(sample)
    torch.testing.assert_close(reference_result, hf_result, rtol=1e-3, atol=1e-3)
    del source, reference, weights, hf_result
    gc.collect()
    encoder = AudioEncoderANE(*dims).eval()
    encoder._register_load_state_dict_pre_hook(linear_to_conv2d_map)
    encoder.load_state_dict(mapped, strict=True)
    del mapped
    with torch.no_grad():
        ane_result = encoder(sample)
        torch.testing.assert_close(ane_result, reference_result, rtol=1e-3, atol=1e-3)
        traced = torch.jit.trace(encoder, sample)
    ml = ct.convert(traced, convert_to='mlprogram', minimum_deployment_target=ct.target.macOS13,
                    inputs=[ct.TensorType(name='logmel_data', shape=sample.shape, dtype=np.float32)],
                    outputs=[ct.TensorType(name='output', dtype=np.float32)],
                    compute_precision=ct.precision.FLOAT16,
                    compute_units=ct.ComputeUnit.ALL, skip_model_load=True)
    del traced, encoder, ane_result
    gc.collect()
    ml.user_defined_metadata['precision'] = 'float16'
    ml.user_defined_metadata['source_model'] = args.model
    ml.user_defined_metadata['source_revision'] = revision or 'local snapshot; see encoder_weights_sha256'
    ml.user_defined_metadata['encoder_weights_sha256'] = digest.hexdigest()
    ml.user_defined_metadata['whisper_cpp_revision'] = VENDOR
    ml.save(str(package))
    # Verify actual Core ML predictions, not just conversion success. Force CPU
    # here for reproducible numerical checking; app placement is profiled separately.
    checked = ct.models.MLModel(str(package), compute_units=ct.ComputeUnit.CPU_ONLY)
    result = checked.predict({'logmel_data': sample.numpy()})['output']
    if result.shape != (1, cfg.max_source_positions, cfg.d_model) or not np.isfinite(result).all():
        raise ValueError(f'Invalid Core ML output: {result.shape}')
    ref = reference_result.numpy()
    relative_error = float(np.linalg.norm(result - ref) / max(np.linalg.norm(ref), 1e-12))
    print(f'Core ML/FP32 random-input relative L2 error: {relative_error:.6f}; '
          f'acceptance limit: {args.max_relative_l2_error:.6f}', flush=True)
    if relative_error > args.max_relative_l2_error:
        raise ValueError(f'Core ML relative L2 error too large: {relative_error}')
    with tempfile.TemporaryDirectory(dir=args.output_dir) as tmp:
        subprocess.run(['xcrun', 'coremlc', 'compile', str(package.resolve()), tmp], check=True)
        shutil.move(str(Path(tmp) / compiled.name), str(compiled))
    manifest.write_text(json.dumps(dict(source_model=args.model, source_revision=revision,
        encoder_weights_sha256=digest.hexdigest(), whisper_cpp_revision=VENDOR,
        matching_ggml_name=args.ggml_name, encoder_dimensions=dims,
        coreml_relative_l2_error=relative_error,
        validation_max_relative_l2_error=args.max_relative_l2_error, precision='float16', torch=torch.__version__, coremltools=ct.__version__), indent=2) + '\n')
    print(f'Validated encoder: {compiled}\nProvenance: {manifest}')


if __name__ == '__main__':
    main()
