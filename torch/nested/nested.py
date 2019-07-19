import torch

# Set this flag to true, if you want to enable additional verifications.
DEBUG = False

# This implementation is based on NestedTensor 0.0.1
# RFC: https://github.com/pytorch/pytorch/issues/22169

def is_nested_tensor(obj):
    return isinstance(obj, NestedTensor)


# Arguments match torch.tensor
def make_nested_tensor(data, dtype=None, device=None, requires_grad=False, pin_memory=False):
    if is_nested_tensor(data):
        # This is consistent with torch.tensor(torch.Tensor)
        # but errors out.
        raise ValueError("To copy construct from a NestedTensor, "
                         "use sourceTensor.clone().detach() or "
                         "sourceTensor.clone().detach().requires_grad_(True), "
                         "rather than torch.tensor(sourceTensor).")
    elif torch.is_tensor(data):
        # The user has the right to expect a NestedTensor from this
        # function, but we can't meaningfully provide one if passed a Tensor
        raise ValueError("Can't construct a NestedTensor from a Tensor")
    else:
        if not (isinstance(data, list) or isinstance(data, tuple)):
            raise ValueError("Pass a list or tuple to construct NestedTensor.")
        for data_ in data:
            if not torch.is_tensor(data_):
                raise ValueError("Each element of the tuple or list must "
                                 "be a torch.Tensor")
        tensors = []
        for data_ in data:
            # torch.tensor copies on construction
            new_data = torch.empty_like(data_)
            new_data.copy_(data_)
            new_data = new_data.to(dtype=dtype, device=device)
            new_data = new_data.requires_grad_(requires_grad)
            if pin_memory:
                new_data = new_data.pin_memory()
            tensors.append(new_data)

        return NestedTensor(tensors)

def as_nestedtensor(data, dtype=None, device=None):
    ret = NestedTensor(data)
    if dtype is not None:
        ret = ret.to(dtype)
    if device is not None:
        ret = ret.to(device)
    return ret

def _verify_tensors(tensors):
    for tensor in tensors:
        assert torch.is_tensor(tensor)
    if len(tensors):
        dim = tensors[0].dim()
        layout = tensors[0].layout
        device = tensors[0].device
        dtype = tensors[0].dtype
        requires_grad = tensors[0].requires_grad
        is_pinned = tensors[0].is_pinned()
        for tensor in tensors:
            if not (dim == tensor.dim() and
                    layout == tensor.layout and
                    device == tensor.device and
                    dtype == tensor.dtype and
                    requires_grad == tensor.requires_grad and
                    is_pinned == tensor.is_pinned()):
                raise ValueError("Each passed Tensor "
                                 "must match in dim, layout, "
                                 "device, dtype and requires_grad")
    else:
        # Carrying around information as member variables vs.
        # checking one entry of the owned Tensors is annoying
        # and error-prone. Carrying around an is_empty attribute
        # to hide the fact that we carry around a list with a
        # single empty Tensor is also annoying and error-prone.
        # Both are not worth it for a minor feature.
        raise ValueError("We do not support empty lists for now.")

class NestedTensor(object):
    # The attributes must match across all constiuents
    #
    # The NestedTensor's attributes then become that of its
    # constiuents.
    #
    # The passed lists of tensors must be non-empty for now.
    #
    # Attributes:
    #     dim
    #     layout
    #     device
    #     dtype
    #     requires_grad
    #     is_pinned
    def __init__(self, tensors):
        self._tensors = tensors
        _verify_tensors(self._tensors)

    @property
    def grad(self):
        grads = [t.grad for t in self._tensors]
        if any(grad is None for grad in grads):
            assert all(grad is None for grad in grads)
            return None
        else:
            return NestedTensor(grads)

    @property
    def data(self):
        return NestedTensor([t.data for t in self._tensors])

    @property
    def dim(self):
        if DEBUG:
            _verify_tensors(self._tensors)
        return self._tensors[0].dim

    @property
    def shape(self):
        raise NotImplementedError()

    @property
    def requires_grad(self):
        if DEBUG:
            _verify_tensors(self._tensors)
        return self._tensors[0].requires_grad

    @property
    def grad_fn(self):
        raise NotImplementedError(
            "We don't support grad_fn as a user-facing construct.")

    def __len__(self):
        return len(self._tensors)

    def __bool__(self):
        raise NotImplementedError(
            "This has not been covered by NestedTensor 0.0.1")

    def __str__(self):
        result = "nestedtensor([\n"
        for tensor in self._tensors:
            result += "  " + tensor.__str__() + ",\n"
        result += "])"
        return result

    def __repr__(self):
        result = "nestedtensor([\n"
        for tensor in self._tensors:
            result += "  " + tensor.__repr__() + ",\n"
        result += "])"
        return result

    def is_empty(self):
        # This condition can never be true, since we disallow an empty list for now.
        raise ValueError("self._tensors cannot be empty under current constraints.")

    def __apply(self, fn):
        return [fn(tensor) for tensor in self._tensors]

    def nested_size(self):
        return tuple(t.size() for t in self._tensors)

    # TODO: Not covered by RFC! NestedTensor 0.0.2 will talk about reductions.
    def all(self):
        return all(t.all() for t in self._tensors)

    # TODO: Not covered by RFC! NestedTensor 0.0.2 will talk about reductions.
    def any(self):
        return any(t.any() for t in self._tensors)

    # TODO: Not covered by RFC! NestedTensor 0.0.2 will talk about reductions.
    def sum(self):
        # We currently assume len(self._tensors) is always non-zero
        return torch.stack(tuple(t.sum() for t in self._tensors)).sum()

    # Tensor ops
    def detach(self):
        return NestedTensor(self.__apply(lambda x: x.detach()))

    def detach_(self):
        return NestedTensor(self.__apply(lambda x: x.detach_()))

    def clone(self):
        return NestedTensor(self.__apply(lambda x: x.clone()))

    def to(self, *args, **kwargs):
        return NestedTensor(self.__apply(lambda x: x.to(*args, **kwargs)))

    def requires_grad_(self, *args, **kwargs):
        return NestedTensor(self.__apply(lambda x: x.requires_grad_(*args, **kwargs)))

    def backward(self, *args, **kwargs):
        self.__apply(lambda x: x.backward(*args, **kwargs))
