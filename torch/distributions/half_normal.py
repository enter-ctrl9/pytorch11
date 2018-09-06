import math

import torch
from torch._six import inf
from torch.distributions import constraints
from torch.distributions.transforms import AbsTransform
from torch.distributions.normal import Normal
from torch.distributions.transformed_distribution import TransformedDistribution


class HalfNormal(TransformedDistribution):
    r"""
    Creates a half-normal distribution parameterized by `scale` where::

        X ~ Normal(0, scale)
        Y = |X| ~ HalfNormal(scale)

    Example::

        >>> m = HalfNormal(torch.tensor([1.0]))
        >>> m.sample()  # half-normal distributed with scale=1
        tensor([ 0.1046])

    Args:
        scale (float or Tensor): scale of the full Normal distribution
    """
    arg_constraints = {'scale': constraints.positive}
    support = constraints.positive
    has_rsample = True

    def __init__(self, scale, validate_args=None):
        base_dist = Normal(0, scale)
        super(HalfNormal, self).__init__(base_dist, AbsTransform(),
                                         validate_args=validate_args)

    def expand(self, batch_shape, instance=None):
        if not instance and type(self).__init__ is not HalfNormal.__init__:
            raise NotImplementedError("Subclasses that define a custom __init__ method "
                                      "must also define a custom .expand() method")
        batch_shape = torch.Size(batch_shape)
        new = self.__new__(type(self)) if not instance else instance
        base_dist = self.base_dist.expand(batch_shape)
        super(HalfNormal, new).__init__(base_dist, AbsTransform(), validate_args=False)
        new._validate_args = self._validate_args
        return new

    @property
    def scale(self):
        return self.base_dist.scale

    @property
    def mean(self):
        return self.scale * math.sqrt(2 / math.pi)

    @property
    def variance(self):
        return self.scale.pow(2) * (1 - 2 / math.pi)

    def log_prob(self, value):
        log_prob = self.base_dist.log_prob(value) + math.log(2)
        log_prob[value.expand(log_prob.shape) < 0] = -inf
        return log_prob

    def cdf(self, value):
        return 2 * self.base_dist.cdf(value) - 1

    def icdf(self, prob):
        return self.base_dist.icdf((prob + 1) / 2)

    def entropy(self):
        return self.base_dist.entropy() - math.log(2)
