"""cmaes.py -- CMA-ES in a box, in numpy: the search tier A of ImpAI's tuning runs (tune.py).

The (mu/mu_w, lambda)-CMA-ES of Hansen's tutorial ("The CMA Evolution Strategy: A Tutorial", 2016, the default
parameters of its Table 1), minimising, in coordinates normalised to the box [-1, 1]^d. A sample outside the box
is evaluated at its projection onto the box and its fitness is raised by PENALTY times its squared distance
from it, so the search is pushed back inside without ever evaluating a point outside (Hansen's simple
boundary handling); the update itself uses the unprojected sample.

DETERMINISTIC FROM ITS SEED: every draw comes from one numpy Generator (PCG64) seeded once, and the whole search
-- the distribution, the paths, the generator's state, the generation -- is a JSON-able dict (state(),
from_state()), so a search stopped after any tell and resumed from its state draws and updates exactly as one
that never stopped. No clock is read."""
import math

import numpy as np

PENALTY = 1.0       # fitness added per unit of squared distance outside the box (the box is [-1, 1] wide)


class CMAES:
    def __init__(self, dim: int, popsize: int, seed: int, sigma: float = 0.3, mean=None):
        if dim < 1 or popsize < 2:
            raise ValueError("CMA-ES needs a dimension of at least 1 and a population of at least 2")
        self.dim, self.lam, self.seed = dim, popsize, seed
        self.rng = np.random.Generator(np.random.PCG64(seed))
        self.mean = np.zeros(dim) if mean is None else np.clip(np.asarray(mean, float), -1.0, 1.0)
        self.sigma = float(sigma)
        self.C = np.eye(dim)
        self.pc = np.zeros(dim)
        self.ps = np.zeros(dim)
        self.gen = 0
        self.evals = 0
        self._params()

    def _params(self):
        n, lam = self.dim, self.lam
        mu = lam // 2
        w = np.log((lam + 1) / 2.0) - np.log(np.arange(1, mu + 1))
        self.weights = w / w.sum()
        self.mu = mu
        self.mueff = 1.0 / float(np.sum(self.weights ** 2))
        self.cc = (4 + self.mueff / n) / (n + 4 + 2 * self.mueff / n)
        self.cs = (self.mueff + 2) / (n + self.mueff + 5)
        self.c1 = 2 / ((n + 1.3) ** 2 + self.mueff)
        self.cmu = min(1 - self.c1, 2 * (self.mueff - 2 + 1 / self.mueff) / ((n + 2) ** 2 + self.mueff))
        self.damps = 1 + 2 * max(0.0, math.sqrt((self.mueff - 1) / (n + 1)) - 1) + self.cs
        self.chin = math.sqrt(n) * (1 - 1 / (4 * n) + 1 / (21 * n * n))

    def _eig(self):
        C = (self.C + self.C.T) / 2.0
        d2, B = np.linalg.eigh(C)
        d2 = np.maximum(d2, 1e-20)
        return B, np.sqrt(d2)

    def ask(self) -> np.ndarray:
        """The generation's lambda samples, one a row, in normalised coordinates (possibly outside the box)."""
        B, D = self._eig()
        z = self.rng.standard_normal((self.lam, self.dim))
        return self.mean + self.sigma * (z * D) @ B.T

    @staticmethod
    def project(y: np.ndarray) -> np.ndarray:
        """The point a sample is evaluated at: its projection onto the box."""
        return np.clip(y, -1.0, 1.0)

    @staticmethod
    def penalty(y: np.ndarray) -> float:
        return PENALTY * float(np.sum((y - np.clip(y, -1.0, 1.0)) ** 2))

    def tell(self, ys: np.ndarray, fs) -> None:
        """The samples `ask` gave, in its order, and their fitness at their projections (lower is better)."""
        ys = np.asarray(ys, float)
        if ys.shape != (self.lam, self.dim) or len(fs) != self.lam:
            raise ValueError("tell takes the generation ask gave, all of it")
        f = np.array([float(fs[i]) + self.penalty(ys[i]) for i in range(self.lam)])
        if not np.all(np.isfinite(f)):
            raise ValueError("a fitness is not a finite number")
        order = np.argsort(f, kind="stable")         # a tie keeps the lower sample index first
        sel = ys[order[:self.mu]]
        old = self.mean
        self.mean = self.weights @ sel
        B, D = self._eig()
        invsqrt = B @ np.diag(1.0 / D) @ B.T
        n = self.dim
        step = (self.mean - old) / self.sigma
        self.ps = (1 - self.cs) * self.ps + math.sqrt(self.cs * (2 - self.cs) * self.mueff) * (invsqrt @ step)
        norm_ps = float(np.linalg.norm(self.ps))
        self.gen += 1
        self.evals += self.lam
        hsig = norm_ps / math.sqrt(1 - (1 - self.cs) ** (2 * self.gen)) / self.chin < 1.4 + 2 / (n + 1)
        self.pc = (1 - self.cc) * self.pc + (hsig * math.sqrt(self.cc * (2 - self.cc) * self.mueff)) * step
        art = (sel - old) / self.sigma
        rank_mu = (art.T * self.weights) @ art
        delta = (1 - hsig) * self.cc * (2 - self.cc)
        self.C = ((1 - self.c1 - self.cmu) * self.C + self.c1 * (np.outer(self.pc, self.pc) + delta * self.C)
                  + self.cmu * rank_mu)
        self.sigma *= math.exp((self.cs / self.damps) * (norm_ps / self.chin - 1))

    def state(self) -> dict:
        return {"dim": self.dim, "lam": self.lam, "seed": self.seed, "mean": self.mean.tolist(),
                "sigma": self.sigma, "C": self.C.tolist(), "pc": self.pc.tolist(), "ps": self.ps.tolist(),
                "gen": self.gen, "evals": self.evals, "rng": self.rng.bit_generator.state}

    @classmethod
    def from_state(cls, s: dict) -> "CMAES":
        es = cls(s["dim"], s["lam"], s["seed"], s["sigma"], s["mean"])
        es.mean = np.array(s["mean"], float)
        es.C = np.array(s["C"], float)
        es.pc = np.array(s["pc"], float)
        es.ps = np.array(s["ps"], float)
        es.gen, es.evals = s["gen"], s["evals"]
        es.rng.bit_generator.state = s["rng"]
        return es
