from collections import namedtuple

import torch
from torch.autograd import Variable


PackedSequence_ = namedtuple('PackedSequence', ['data', 'batch_sizes'])


class PackedSequence(PackedSequence_):
    r"""Holds the data and list of batch_sizes of a packed sequence.

    All RNN modules accept packed sequences as inputs.

    Note:
        Instances of this class should never be created manually. They are meant
        to be instantiated by functions like :func:`pack_padded_sequence`.

    Attributes:
        data (Variable): Variable containing packed sequence
        batch_sizes (list[int]): list of integers holding information about
            the batch size at each sequence step
    """
    pass


def pack_padded_sequence(input, lengths, batch_first=False):
    r"""Packs a Variable containing padded sequences of variable length.

    Input can be of size ``TxBx*`` where T is the length of the longest sequence
    (equal to ``lengths[0]``), B is the batch size, and * is any number of
    dimensions (including 0). If ``batch_first`` is True ``BxTx*`` inputs are
    expected.

    The sequences should be sorted by length in a decreasing order, i.e.
    ``input[:,0]`` should be the longest sequence, and ``input[:,B-1]`` the
    shortest one.

    Note:
        This function accept any input that has at least two dimensions. You
        can apply it to pack the labels, and use the output of the RNN with
        them to compute the loss directly. A Variable can be retrieved from
        a :class:`PackedSequence` object by accessing its ``.data`` attribute.

    Arguments:
        input (Variable): padded batch of variable length sequences.
        lengths (list[int]): list of sequences lengths of each batch element.
        batch_first (bool, optional): if ``True``, the input is expected in BxTx*
            format.

    Returns:
        a :class:`PackedSequence` object
    """
    if lengths[-1] <= 0:
        raise ValueError("length of all samples has to be greater than 0, "
                         "but found an element in 'lengths' that is <=0")
    if batch_first:
        input = input.transpose(0, 1)

    steps = []
    batch_sizes = []
    lengths_iter = reversed(lengths)
    batch_size = input.size(1)
    if len(lengths) != batch_size:
        raise ValueError("lengths array has incorrect size")

    prev_l = 0
    for i, l in enumerate(lengths_iter):
        if l > prev_l:
            c_batch_size = batch_size - i
            steps.append(input[prev_l:l, :c_batch_size].contiguous().view(-1, *input.size()[2:]))
            batch_sizes.extend([c_batch_size] * (l - prev_l))
            prev_l = l
        elif prev_l > l:  # remember that new_length is the preceding length in the array
            raise ValueError("lengths array has to be sorted in decreasing order")

    return PackedSequence(torch.cat(steps), batch_sizes)


def pad_packed_sequence(sequence, batch_first=False, padding_value=0.0):
    r"""Pads a packed batch of variable length sequences.

    It is an inverse operation to :func:`pack_padded_sequence`.

    The returned Variable's data will be of size TxBx*, where T is the length
    of the longest sequence and B is the batch size. If ``batch_first`` is True,
    the data will be transposed into BxTx* format.

    Batch elements will be ordered decreasingly by their length.

    Arguments:
        sequence (PackedSequence): batch to pad
        batch_first (bool, optional): if ``True``, the output will be in BxTx*
            format.
        padding_value (float, optional): values for padded elements

    Returns:
        Tuple of Variable containing the padded sequence, and a list of lengths
        of each sequence in the batch.
    """
    var_data, batch_sizes = sequence
    max_batch_size = batch_sizes[0]
    output = var_data.data.new(len(batch_sizes), max_batch_size, *var_data.size()[1:]).fill_(padding_value)
    output = Variable(output)

    lengths = []
    data_offset = 0
    prev_batch_size = batch_sizes[0]
    prev_i = 0
    for i, batch_size in enumerate(batch_sizes + [0]):
        if batch_size != prev_batch_size:
            l = prev_batch_size * (i - prev_i)
            tmp = var_data[data_offset:data_offset + l]
            output[prev_i:i, :prev_batch_size] = tmp.view(i - prev_i, prev_batch_size, *tmp.size()[1:])
            data_offset += l
            prev_i = i
        dec = prev_batch_size - batch_size
        if dec > 0:
            lengths.extend((i,) * dec)
        prev_batch_size = batch_size

    lengths.reverse()

    if batch_first:
        output = output.transpose(0, 1)
    return output, lengths


def pad_sequence(sequences, lengths, batch_first=False):
    r"""Pad a list of variable length Variables with zero

    The ``pad_sequence`` pads the list of Variables on zeroth dimension and
    stack all the sequences on zeroth dimension. For example, if the input is
    list of sequences with size `` Lx*`` and if batch_first is False, the
    output will be of size `` TxBx* `` and if batch_first is True,
    output will be of size ``BxTx* ``. The ``pad_sequence`` accepts list of
    sequences and its lengths which should be sorted in decreasing order

    B is batch size - Number of elements in ``sequences``
    T is length longest sequence
    L is length of the sequence
    * is any number of trailing dimensions, including none

    >>> from torch.nn.utils.rnn import pad_sequence
    >>> a = Variable(torch.ones(25, 300))
    >>> b = Variable(torch.ones(22, 300))
    >>> c = Variable(torch.ones(15, 300))
    >>> pad_sequence([a, b, c], [25, 22, 15]).size()
    torch.Size([25, 3, 300])

    Note:
        This function returns a Variable of size TxBx* or BxTx* where T is the
            length of longest sequence (lengths[0])
        Function assumes trailing dimensions and type of all the Variables
            in sequences are same

    Arguments:
        sequences (list[Variable]): list of variable length sequences.
        lengths (list[int]): list of sequences lengths of each batch element.
        batch_first (bool, optional): if True, the input is expected in Bx*x*
            format.

    Returns:
        Variable of size ``T x B x * `` if batch_first = False
        Variable of size ``B x T x * `` otherwise
    """

    if len(lengths) != len(sequences):
        raise ValueError("number of elements in lengths and sequences didn't match")

    # assuming trailing dimensions and type of all the Variables
    # in sequences are same and fetching those from sequences[0]
    max_len = lengths[0]
    trailing_dims = list(sequences[0].size())[1:]
    prev_l = max_len
    if batch_first:
        out_dims = [len(sequences), max_len] + trailing_dims
        out_variable = Variable(sequences[0].data.new(*out_dims).zero_())
        batch_dim = 0
    else:
        out_dims = [max_len, len(sequences)] + trailing_dims
        out_variable = Variable(sequences[0].data.new(*out_dims).zero_())
        batch_dim = 1

    for i, variable, length in zip(range(len(lengths)), sequences, lengths):
        # temperory sort check, can be removed when we handle sorting internally
        if prev_l < length:
                raise ValueError("lengths array has to be sorted in decreasing order")
        prev_l = length
        out_variable.select(batch_dim, i)[:length] = variable
    return out_variable


def pack_sequence(sequences, lengths):
    r"""Packs a list of variable length Variables

    sequences should be a list of Variables each has size `` Lx* `` where
    L is length of the sequence and * is any trailing dimension including zero
    ``pack_sequence`` assumes each Variable is of different length and pack them

    Note:
        sequences can have any input that has at least one dimension.
        But ``pack_sequence`` assumes, first dimension has varaible length
        sequences. You can apply it to pack the labels, and use the output of
        the RNN with them to compute the loss directly. A Variable can be
        retrieved from a :class:`PackedSequence` object by accessing
        its ``.data`` attribute.
    Ex.
        >>> from torch.nn.utils.rnn import pack_sequence
        >>> a = Variable(torch.Tensor([1,2,3]))
        >>> b = Variable(torch.Tensor([4,5]))
        >>> c = Variable(torch.Tensor([6]))
        >>> pack_sequence([a, b, c], [3, 2, 1])
        PackedSequence(data=
         1
         4
         6
         2
         5
         3
        [torch.FloatTensor of size 6]
        , batch_sizes=[3, 2, 1])


    Arguments:
        sequences (list[Variable]): variable length sequences.
        lengths (list[int]): list of sequence's lengths of each batch element.

    Returns:
        a :class:`PackedSequence` object
    """

    return pack_padded_sequence(pad_sequence(sequences, lengths), lengths)
