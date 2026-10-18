# PP-OCRv5 & PP-LCNet Models Guide

> [!NOTE]
> **Avoid Repository Bloat**: Heavy ONNX model weights (`*.onnx`) are distributed outside Git. You can obtain all required model files directly by extracting them from the official downloaded plugin ZIP package.

Place the following ONNX model files in this directory:

1. `PP-OCRv5_server_det.onnx` (~88 MB)
   - PaddleOCR PP-OCRv5 Server detection model
   - Dynamic shape: `[1, 3, -1, -1]`
2. `PP-OCRv5_server_rec.onnx` (~81 MB)
   - PaddleOCR PP-OCRv5 Server recognition model
   - Dynamic width: `[1, 3, 48, -1]`
3. `PP-LCNet_x1_0_textline_ori.onnx` (~7 MB)
   - PP-LCNet textline orientation classification (0° / 180°)
   - Input shape: `[1, 3, 48, 192]`
4. `ppocrv5_dict.txt` (18,383 tokens)
   - PaddleOCR v5 character dictionary

## Export with Paddle2ONNX (Official Tooling)

If converting from official PaddlePaddle weights:

```bash
# 1. Detection model (dynamic H and W)
paddle2onnx --model_dir ./ch_PP-OCRv5_server_det_infer \
            --model_filename inference.pdmodel \
            --params_filename inference.pdiparams \
            --save_file ./PP-OCRv5_server_det.onnx \
            --opset_version 17 \
            --enable_onnx_checker True

# 2. Recognition model (dynamic W)
paddle2onnx --model_dir ./ch_PP-OCRv5_server_rec_infer \
            --model_filename inference.pdmodel \
            --params_filename inference.pdiparams \
            --save_file ./PP-OCRv5_server_rec.onnx \
            --opset_version 17 \
            --enable_onnx_checker True

# 3. Orientation model (fixed 3x48x192)
paddle2onnx --model_dir ./ch_PP-LCNet_x1_0_textline_ori_infer \
            --model_filename inference.pdmodel \
            --params_filename inference.pdiparams \
            --save_file ./PP-LCNet_x1_0_textline_ori.onnx \
            --opset_version 17 \
            --enable_onnx_checker True
```

Once placed in this directory, run `..\package.ps1` to produce the full ZIP archive.
