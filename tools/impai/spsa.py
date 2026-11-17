"""spsa.py -- SPSA in the unit box, in numpy: the search of tune.py's `spsa` runs.

Spall's simultaneous perturbation stochastic approximation ("Implementation of the Simultaneous Perturbation
Algorithm for Stochastic Optimization", IEEE Trans. AES 34(3), 1998), MAXIMISING an objective J over theta in
[0, 1]^d. Iteration k (from 0):

    c_k = c / (k + 1)^gamma                 the perturbation's size
    a_k = a / (k + 1 + A)^alpha             the step's gain
    delta_k in {-1, +1}^d                   Rademacher, one draw a coordinate
    theta+ = clip(theta + c_k delta_k), theta- = clip(theta - c_k delta_k)
    g_k = (J(theta+) - J(theta-)) / (2 c_k) * delta_k          (1 / delta_i = delta_i)
    theta <- clip(theta + clip(a_k g_k, -max_step, max_step))

with Spall's exponents alpha 0.602 and gamma 0.101 and his stability constant A, a tenth of the iterations. The
two evaluations of an iteration are COMMON RANDOM NUMBERS: the caller plays theta+ and theta- on the same games,
so the difference holds the perturbation's effect and not the games' spread; an objective may give the
difference itself (`diff`), a paired comparison of the two arms, in place of two values. A step is held to
max_step a coordinate so one noisy difference cannot throw theta across the box.

DETERMINISTIC FROM ITS SEED: delta_k is drawn from a PCG64 generator seeded by (seed, k) alone, so an iteration's
perturbation does not depend on how many were drawn before it, and the whole search is a JSON-able dict
(state(), from_state()): a search stopped after any `tell` and resumed from its state goes on exactly as one that
never stopped. No clock is read."""
import numpy as np

ALPHA = 0.602
GAMMA = 0.101


def default_gain(iters: int, c: float, step0: float = 0.1, dj0: float = 0.2) -> float:
    """Spall's a: the first step of a coordinate step0 when the first difference is dj0 (A = iters / 10)."""
    A = iters / 10.0
    return step0 * (1.0 + A) ** ALPHA * 2.0 * c / dj0


class SPSA:
    def __init__(self, dim: int, seed: int, iters: int, theta0, a: float = None, c: float = 0.15,
                 max_step: float = 0.2):
        if dim < 1 or iters < 1:
            raise ValueError("SPSA needs a dimension of at least 1 and at least one iteration")
        if not 0.0 < c <= 0.5:
            raise ValueError("the perturbation c is a share of the unit box, in (0, 0.5]")
        self.dim, self.seed, self.iters = dim, int(seed), int(iters)
        self.c, self.max_step = float(c), float(max_step)
        self.A = iters / 10.0
        self.a = default_gain(iters, c) if a is None else float(a)
        self.theta = np.clip(np.asarray(theta0, float), 0.0, 1.0)
        if self.theta.shape != (dim,):
            raise ValueError(f"theta0 has {self.theta.size} coordinates, not {dim}")
        self.k = 0

    def gains(self, k: int) -> tuple:
        """(a_k, c_k)."""
        return self.a / (k + 1 + self.A) ** ALPHA, self.c / (k + 1) ** GAMMA

    def delta(self, k: int) -> np.ndarray:
        rng = np.random.Generator(np.random.PCG64([self.seed, k]))
        return rng.integers(0, 2, self.dim) * 2.0 - 1.0

    def ask(self) -> tuple:
        """(theta+, theta-, delta) of the current iteration."""
        _, ck = self.gains(self.k)
        d = self.delta(self.k)
        return np.clip(self.theta + ck * d, 0.0, 1.0), np.clip(self.theta - ck * d, 0.0, 1.0), d

    def tell(self, diff: float) -> np.ndarray:
        """The iteration's J(theta+) - J(theta-): the step taken, theta moved, k advanced. Returns the step."""
        ak, ck = self.gains(self.k)
        g = float(diff) / (2.0 * ck) * self.delta(self.k)
        step = np.clip(ak * g, -self.max_step, self.max_step)
        self.theta = np.clip(self.theta + step, 0.0, 1.0)
        self.k += 1
        return step

    def done(self) -> bool:
        return self.k >= self.iters

    def state(self) -> dict:
        return {"dim": self.dim, "seed": self.seed, "iters": self.iters, "a": self.a, "c": self.c,
                "max_step": self.max_step, "theta": self.theta.tolist(), "k": self.k}

    @classmethod
    def from_state(cls, s: dict) -> "SPSA":
        o = cls(s["dim"], s["seed"], s["iters"], s["theta"], a=s["a"], c=s["c"], max_step=s["max_step"])
        o.k = int(s["k"])
        return o
