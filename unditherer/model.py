"""The restorer: a DnCNN-style residual CNN, deterministic and 1x.

Input and output are RGB in [0,1]; the network predicts the correction
(dither / banding residual) and the output is input minus that.  Depth and
width are the two knobs: (12, 64) is the "full" model, (6, 24) the "tiny" one
small enough to consider porting to fragment shaders.
"""
import torch
import torch.nn as nn


class Restorer(nn.Module):
    def __init__(self, depth=12, ch=64, padding_mode="zeros"):
        super().__init__()
        layers = [nn.Conv2d(3, ch, 3, padding=1, padding_mode=padding_mode), nn.ReLU(inplace=True)]
        for _ in range(depth - 2):
            layers += [nn.Conv2d(ch, ch, 3, padding=1, padding_mode=padding_mode, bias=False),
                       nn.BatchNorm2d(ch), nn.ReLU(inplace=True)]
        layers += [nn.Conv2d(ch, 3, 3, padding=1, padding_mode=padding_mode)]
        self.net = nn.Sequential(*layers)
        self.depth, self.ch = depth, ch
        for m in self.modules():
            if isinstance(m, nn.Conv2d):
                nn.init.kaiming_normal_(m.weight, nonlinearity="relu")
                if m.bias is not None:
                    nn.init.zeros_(m.bias)
        nn.init.zeros_(self.net[-1].weight)          # start as the identity

    def forward(self, x):
        return x - self.net(x)

    def receptive_field(self):
        return 2 * self.depth + 1


def count_params(model):
    return sum(p.numel() for p in model.parameters())


def flops_per_pixel(depth, ch):
    """Multiply-accumulates per output pixel (x2 for FLOPs), 3x3 convs only."""
    macs = 9 * 3 * ch + 9 * ch * ch * (depth - 2) + 9 * ch * 3
    return 2 * macs
