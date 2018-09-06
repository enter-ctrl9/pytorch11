import math
from numbers import Number

import torch
from torch.distributions import constraints
from torch.distributions.distribution import Distribution
from torch.distributions.utils import broadcast_all


class Uniform(Distribution):
    r"""
    Generates uniformly distributed random samples from the half-open interval
    ``[low, high)``.

    Example::

        >>> m = Uniform(torch.tensor([0.0]), torch.tensor([5.0]))
        >>> m.sample()  # uniformly distributed in the range [0.0, 5.0)
        tensor([ 2.3418])

    Args:
        low (float or Tensor): lower range (inclusive).
        high (float or Tensor): upper range (exclusive).
    """
    # TODO allow (loc,scale) parameterization to allow independent constraints.
    arg_constraints = {'low': constraints.dependent, 'high': constraints.dependent}
    has_rsample = True

    @property
    def mean(self):
        return (self.high + self.low) / 2

    @property
    def stddev(self):
        return (self.high - self.low) / 12**0.5

    @property
    def variance(self):
        return (self.high - self.low).pow(2) / 12

    def __init__(self, low, high, validate_args=None):
        self.low, self.high = broadcast_all(low, high)

        if isinstance(low, Number) and isinstance(high, Number):
            batch_shape = torch.Size()
        else:
            batch_shape = self.low.size()
        super(Uniform, self).__init__(batch_shape, validate_args=validate_args)

        if self._validate_args and not torch.lt(self.low, self.high).all():
            raise ValueError("Uniform is not defined when low>= high")

    def expand(self, batch_shape, instance=None):
        if not instance and type(self).__init__ is not Uniform.__init__:
            raise NotImplementedError("Subclasses that define a custom __init__ method "
                                      "must also define a custom .expand() method")
        new = self.__new__(type(self)) if not instance else instance
        batch_shape = torch.Size(batch_shape)
        new.low = self.low.expand(batch_shape)
        new.high = self.high.expand(batch_shape)
        super(Uniform, new).__init__(batch_shape, validate_args=False)
        new._validate_args = self._validate_args
        return new

    @constraints.dependent_property
    def support(self):
        return constraints.interval(self.low, self.high)

    def rsample(self, sample_shape=torch.Size()):
        shape = self._extended_shape(sample_shape)
        rand = self.low.new(shape).uniform_()
        return self.low + rand * (self.high - self.low)

    def log_prob(self, value):
        if self._validate_args:
            self._validate_sample(value)
        lb = value.ge(self.low).type_as(self.low)
        ub = value.lt(self.high).type_as(self.low)
        return torch.log(lb.mul(ub)) - torch.log(self.high - self.low)

    def cdf(self, value):
        if self._validate_args:
            self._validate_sample(value)
        result = (value - self.low) / (self.high - self.low)
        return result.clamp(min=0, max=1)

    def icdf(self, value):
        if self._validate_args:
            self._validate_sample(value)
        result = value * (self.high - self.low) + self.low
        return result

    def entropy(self):
        return torch.log(self.high - self.low)
