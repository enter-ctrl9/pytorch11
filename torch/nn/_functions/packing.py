import torch
from torch.autograd import Function


class PackPadded(Function):
    @staticmethod
    def forward(ctx, input, lengths):

        if lengths[-1] <= 0:
            raise ValueError("Length of all samples has to be greater than 0, "
                             "but found an element in 'lengths' that is <= 0")

        steps = []
        batch_sizes = []
        lengths_iter = reversed(lengths)
        batch_size = input.size(1)
        
        if len(lengths) != batch_size:
            raise ValueError("Expected `len(lengths)` to be equal to batch_size, but got "
                             "{} (batch_size={}).".format(len(lengths), batch_size))

        prev_l = 0
        for i, l in enumerate(lengths_iter):
            if l > prev_l:
                c_batch_size = batch_size - i
                steps.append(input[prev_l:l, :c_batch_size].contiguous().view(-1, *input.size()[2:]))
                batch_sizes.extend([c_batch_size] * (l - prev_l))
                prev_l = l
                
            elif prev_l > l:  
                raise ValueError("'lengths' array has to be sorted in decreasing order")

        ctx.batch_sizes = batch_sizes
        ctx.input_size = input.size()
                
        return torch.cat(steps), torch.LongTensor(batch_sizes)

    @staticmethod
    def backward(ctx, grad_steps, grad_batch_sizes):
        
        idx = 0
        ret = grad_steps.new(*ctx.input_size).zero_()
        
        for i, bs in enumerate(ctx.batch_sizes):
            ret[i, :bs, ...] = grad_steps[idx:idx+bs, ...]
            idx += bs
            
        return ret, None
