from numbers import Number
import math
import torch
from torch.distributions import constraints
from torch.distributions.uniform import Uniform
from torch.distributions.transformed_distribution import TransformedDistribution
from torch.distributions.transforms import AffineTransform, ExpTransform
from torch.distributions.utils import _finfo, broadcast_all

euler_constant = 0.57721566490153286060  # Euler Mascheroni Constant


class Gumbel(TransformedDistribution):
    r"""
    Samples from a Gumbel Distribution.

    Examples::

        >>> m = Gumbel(torch.tensor([1.0]), torch.tensor([2.0]))
        >>> m.sample()  # sample from Gumbel distribution with loc=1, scale=2
        tensor([ 1.0124])

    Args:
        loc (float or Tensor): Location parameter of the distribution
        scale (float or Tensor): Scale parameter of the distribution
    """
    arg_constraints = {'loc': constraints.real, 'scale': constraints.positive}
    support = constraints.real

    def __init__(self, loc, scale, validate_args=None):
        self.loc, self.scale = broadcast_all(loc, scale)
        finfo = _finfo(self.loc)
        if isinstance(loc, Number) and isinstance(scale, Number):
            base_dist = Uniform(finfo.tiny, 1 - finfo.eps)
        else:
            base_dist = Uniform(self.loc.new(self.loc.size()).fill_(finfo.tiny), 1 - finfo.eps)
        transforms = [ExpTransform().inv, AffineTransform(loc=0, scale=-torch.ones_like(self.scale)),
                      ExpTransform().inv, AffineTransform(loc=loc, scale=-self.scale)]
        super(Gumbel, self).__init__(base_dist, transforms, validate_args=validate_args)

    def expand(self, batch_shape, instance=None):
        if not instance and type(self).__init__ is not Gumbel.__init__:
            raise NotImplementedError("Subclasses that define a custom __init__ method "
                                      "must also define a custom .expand() method")
        batch_shape = torch.Size(batch_shape)
        new = self.__new__(type(self)) if not instance else instance
        base_dist = self.base_dist.expand(batch_shape)
        transforms = self.transforms
        super(Gumbel, new).__init__(base_dist, transforms, validate_args=False)
        new._validate_args = self._validate_args
        return new

    @property
    def mean(self):
        return self.loc + self.scale * euler_constant

    @property
    def stddev(self):
        return (math.pi / math.sqrt(6)) * self.scale

    @property
    def variance(self):
        return self.stddev.pow(2)

    def entropy(self):
        return self.scale.log() + (1 + euler_constant)
