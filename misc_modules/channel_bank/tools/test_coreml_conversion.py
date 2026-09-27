"""Fast contract checks; --integration also converts a small synthetic encoder."""
import argparse
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from convert_hf_coreml_encoder import encoder_name, rename_encoder_key


class ContractTests(unittest.TestCase):
    def test_runtime_filename(self):
        self.assertEqual(encoder_name('ggml-whisper-large-v3-atc-q5_0.bin'),
                         'ggml-whisper-large-v3-atc-encoder')
        self.assertEqual(encoder_name('ggml-test.bin'), 'ggml-test-encoder')
        with self.assertRaises(ValueError):
            encoder_name('../model.bin')

    def test_encoder_weight_names(self):
        self.assertEqual(rename_encoder_key('layers.0.self_attn.q_proj.weight'),
                         'blocks.0.attn.query.weight')
        self.assertEqual(rename_encoder_key('layers.0.final_layer_norm.bias'),
                         'blocks.0.mlp_ln.bias')
        self.assertEqual(rename_encoder_key('embed_positions.weight'), 'positional_embedding')
        self.assertEqual(rename_encoder_key('layer_norm.weight'), 'ln_post.weight')


def integration(runtime_test=None, ggml_model=None):
    from transformers import WhisperConfig, WhisperForConditionalGeneration
    # Real HF serialization + strict weight mapping + ANE rewrite + Core ML
    # prediction + coremlc compilation, without downloading a stock model.
    with tempfile.TemporaryDirectory(prefix='cb-coreml-test-') as tmp:
        root = Path(tmp)
        fixture = root / 'fixture'
        cfg = WhisperConfig(d_model=32, encoder_layers=1, decoder_layers=1,
            encoder_attention_heads=2, decoder_attention_heads=2,
            encoder_ffn_dim=128, decoder_ffn_dim=128, vocab_size=128,
            bos_token_id=1, eos_token_id=2, pad_token_id=0, decoder_start_token_id=1)
        WhisperForConditionalGeneration(cfg).save_pretrained(fixture)
        subprocess.run([sys.executable, str(Path(__file__).with_name('convert_hf_coreml_encoder.py')),
            '--model', str(fixture), '--ggml-name', 'ggml-synthetic-q5_0.bin',
            '--output-dir', str(root / 'output')], check=True)
        encoder = root / 'output/ggml-synthetic-encoder.mlmodelc'
        assert encoder.is_dir()
        if runtime_test:
            subprocess.run([runtime_test, str(encoder), 'bridge'], check=True)
        if runtime_test and ggml_model:
            model = root / 'ggml-test-q5_0.bin'
            model.symlink_to(Path(ggml_model).resolve())
            expected_encoder = root / 'ggml-test-encoder.mlmodelc'
            for mode in ('disabled', 'fallback'):
                subprocess.run([runtime_test, str(model), mode], check=True)
            expected_encoder.mkdir()  # damaged/incomplete install
            subprocess.run([runtime_test, str(model), 'fallback'], check=True)
            expected_encoder.rmdir()
            expected_encoder.symlink_to(encoder, target_is_directory=True)
            # Valid Core ML package, wrong encoder dimensions for the ATC GGML.
            subprocess.run([runtime_test, str(model), 'fallback'], check=True)


if __name__ == '__main__':
    ap = argparse.ArgumentParser()
    ap.add_argument('--integration', action='store_true')
    ap.add_argument('--runtime-test', help='Compiled test_whisper_coreml executable')
    ap.add_argument('--ggml-model', help='Existing GGML model for isolated fallback checks')
    args = ap.parse_args()
    result = unittest.TextTestRunner().run(unittest.defaultTestLoader.loadTestsFromTestCase(ContractTests))
    if not result.wasSuccessful():
        sys.exit(1)
    if args.integration:
        integration(args.runtime_test, args.ggml_model)
