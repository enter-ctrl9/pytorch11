from __future__ import print_function
import re
import yaml
import pprint
import sys

try:
    # use faster C loader if available
    from yaml import CLoader as Loader
except ImportError:
    from yaml import Loader


# [temp translations]
# We're currently incrementally moving from the custom func schema to the
# JIT signature schema incrementally. This will reduce overall complexity
# and increase compliance between these components. So for now we do simple
# type translations to continue to emit the legacy func schema for further
# processing by downstream tools. This will helps us avoid having to prematurely
# change all downstream tools to detect these new types.
def temp_type_translations(typ):
    # Enables Tensor[] by translating to legacy TensorList. See [temp translations]
    if typ == 'Tensor[]':
        return 'TensorList'
    # Enables int[] by translating to legacy IntList. See [temp translations]
    if typ == 'int[]':
        return 'IntList'
    # Enables int by translating to legacy int64_t. See [temp translations]
    if typ == 'int':
        return 'int64_t'
    # Enables float by translating to legacy double. See [temp translations]
    if typ == 'float':
        return 'double'
    return typ


def parse_default(s):
    if s == 'True':
        return True
    elif s == 'False':
        return False
    elif s == 'true':
        raise RuntimeError("Please use True and not true. "
                    "See [temp translations] for details.")
    elif s == 'false':
        raise RuntimeError("Please use False and not false. "
                    "See [temp translations] for details.")
    elif s == 'nullptr':
        return s
    # Enables default argument [] by translating to legacy {}.
    # See [temp translations]
    elif s == '[]':
        return '{}'
    # Enables lists by translating to legacy {.*}.
    # See [temp translations]
    elif re.match(r'\[.*\]', s):
        return "{" + s[1:-1] + "}"
    elif s == 'None':
        return 'c10::nullopt'
    # The JIT signature schema uses Mean, but in particular C++ needs
    # the legacy Reduction::Mean. So we'll continue emiting that until
    # we change this at either a JIT schema or C++ level.
    elif s == 'Mean':
        return 'Reduction::Mean'
    try:
        return int(s)
    except Exception:
        try:
            return float(s)
        except Exception:
            return s


def sanitize_type(typ):
    if typ == 'Generator*':
        return 'Generator *'
    return typ


def sanitize_types(types):
    # split tuples into constituent list
    if types[0] == '(' and types[-1] == ')':
        return [sanitize_type(x.strip()) for x in types[1:-1].split(',')]
    return [sanitize_type(types)]


def get_annotation(t):
    match = re.match(r'(Tensor.*)\((.+)\)', t)
    annotation = None
    if match:
        t = match.group(1)
        annotation = match.group(2)
    return t, annotation


def parse_arguments(args, func_decl, func_name, func_return, inplace):
    arguments = []
    is_out_fn = func_name.endswith('_out')
    if is_out_fn and func_decl.get('variants', []) not in [[], 'function', ['function']]:
        raise RuntimeError("Native functions suffixed with _out MUST be declared with only the function variant; "
                           "e.g., variants: function; otherwise you will tickle a Python argument binding bug "
                           "(which usually manifests itself as the result variable being undefined.) "
                           "The culprit was: {}".format(func_name))
    kwarg_only = False

    if len(args.strip()) == 0:
        return arguments

    # TODO: Use a real parser here; this will get bamboozled
    # by signatures that contain things like std::array<bool, 2> (note the space)
    for arg_idx, arg in enumerate(args.split(', ')):
        type_and_name = [a.strip() for a in arg.rsplit(' ', 1)]
        if type_and_name == ['*']:
            assert not kwarg_only
            kwarg_only = True
            continue

        t, name = type_and_name
        default = None

        t, annotation = get_annotation(t)

        # Enables Generator? by translating to legacy Generator*. See [temp translations]
        if t == 'Generator?':
            t = 'Generator*'

        if '=' in name:
            ns = name.split('=', 1)
            # This enables Tensor? x=None and translates to legacy
            # "Tensor? x={}". See [temp translations].
            if t == 'Tensor?' and ns[1] == 'None':
                ns[1] = "[]"  # Will translate to {} via parse_default
            # This enables "Generator? x = None and translates to legacy
            # "Generator* x = nullptr". See [temp translations].
            if t == 'Generator*' and ns[1] == 'None':
                ns[1] = 'nullptr'
            name, default = ns[0], parse_default(ns[1])

        typ = sanitize_types(t)
        assert len(typ) == 1
        argument_dict = {'type': typ[0].rstrip('?'), 'name': name, 'is_nullable': typ[0].endswith('?')}
        # Enables int[x] by translating to legacy IntList[x]. See [temp translations]
        match = re.match(r'int\[(\d+)\]', argument_dict['type'])
        if match:
            argument_dict['type'] = 'IntList'
            argument_dict['size'] = int(match.group(1))
        argument_dict['type'] = temp_type_translations(argument_dict['type'])
        if default is not None:
            argument_dict['default'] = default
        # TODO: convention is that the ith-argument correspond to the i-th return, but it would
        # be better if we just named everything and matched by name.
        if is_out_fn and arg_idx < len(func_return):
            argument_dict['output'] = True
        if kwarg_only:
            argument_dict['kwarg_only'] = True

        argument_dict['annotation'] = annotation
        arguments.append(argument_dict)

    # Explicit check for void is a hack and should disappear after a more
    # functionally complete implementation of Tensor aliases.
    if inplace and len(func_return) > 0 and func_return[0]['type'] != "void":
        found_self = False
        for arg_idx, argument in enumerate(arguments):
            if argument['name'] == "self" and inplace:
                assert argument['annotation'] and argument['annotation'].endswith("!"), \
                    "Inplace function \"{}\" needs to annotate Tensor argument named self as mutable.".format(func_decl['func'])
                found_self = True
                assert argument['annotation'] == func_return[arg_idx]['annotation'], \
                        "Inplace function annotations of function {} need to match between input and correponding output.".format(func_decl['func'])
                assert argument['name'] == func_return[arg_idx]['name']
                assert argument['type'] == func_return[arg_idx]['type']
        assert found_self, "Inplace function \"{}\" needs Tensor argument named self.".format(func_decl['func'])
    return arguments


def parse_return_arguments(return_decl, inplace, func_decl):
    arguments = []
    # TODO: Use a real parser here; this will get bamboozled
    # by signatures that contain things like std::array<bool, 2> (note the space)
    if return_decl[0] == '(' and return_decl[-1] == ')':
        return_decl = return_decl[1:-1]
    multiple_args = len(return_decl.split(', ')) > 1

    for arg_idx, arg in enumerate(return_decl.split(', ')):
        type_and_maybe_name = [a.strip() for a in arg.rsplit(' ', 1)]
        # See Note [field_name versus name]
        field_name = None
        if len(type_and_maybe_name) == 1:
            t = type_and_maybe_name[0]
            t, annotation = get_annotation(t)
            if t == "Tensor" and inplace:
                assert annotation and annotation.endswith("!"), "Return Tensor of function \"{}\" flagged as inplace needs to be annotated as mutable".format(func_decl['func'])
                name = 'self'
            else:
                name = 'result' if not multiple_args else 'result' + str(arg_idx)
        else:
            t, name = type_and_maybe_name
            field_name = name
            t, annotation = get_annotation(t)

        typ = sanitize_type(t)
        argument_dict = {'type': typ, 'name': name}
        if field_name is not None:
            argument_dict['field_name'] = field_name
        argument_dict['output'] = True
        argument_dict['type'] = temp_type_translations(argument_dict['type'])
        argument_dict['annotation'] = annotation

        arguments.append(argument_dict)
    return arguments


def has_sparse_dispatches(dispatches):
    for dispatch in dispatches:
        if 'Sparse' in dispatch:
            return True
    return False


def parse_native_yaml(path):
    with open(path, 'r') as f:
        return yaml.load(f, Loader=Loader)


def propagate_field_names(output_arguments, return_arguments):
    if output_arguments:
        for i, r in enumerate(return_arguments):
            if 'field_name' in r:
                output_arguments[i]['field_name'] = r['field_name']


def run(paths):
    declarations = []
    for path in paths:
        for func in parse_native_yaml(path):
            declaration = {'mode': 'native'}
            try:
                declaration['schema_string'] = "aten::" + func['func']
                if '->' in func['func']:
                    func_decl, return_decl = [x.strip() for x in func['func'].split('->')]
                else:
                    raise Exception('Expected return declaration')
                fn_name, arguments = func_decl.split('(', 1)
                assert arguments[-1] == ")", "Expecting closing ) for {}".format(func['func'])
                arguments = arguments[:-1]  # Expect closing )
                declaration['name'] = func.get('name', fn_name)
                declaration['inplace'] = re.search('(^__i|[^_]_$)', fn_name) is not None
                return_arguments = parse_return_arguments(return_decl, declaration['inplace'], func)
                arguments = parse_arguments(arguments, func, declaration['name'], return_arguments, declaration['inplace'])
                output_arguments = [x for x in arguments if x.get('output')]
                propagate_field_names(output_arguments, return_arguments)
                declaration['return'] = return_arguments if len(output_arguments) == 0 else output_arguments
                declaration['variants'] = func.get('variants', ['function'])
                declaration['requires_tensor'] = func.get('requires_tensor', False)
                declaration['matches_jit_signature'] = func.get('matches_jit_signature', False)
                declaration['cpu_half'] = func.get('cpu_half', False)
                declaration['deprecated'] = func.get('deprecated', False)
                declaration['device_guard'] = func.get('device_guard', True)
                declaration['arguments'] = func.get('arguments', arguments)
                declaration['type_method_definition_dispatch'] = func.get('dispatch', declaration['name'])
                declaration['python_module'] = func.get('python_module', '')
                declaration['aten_sparse'] = has_sparse_dispatches(
                    declaration['type_method_definition_dispatch'])
                declarations.append(declaration)
            except Exception as e:
                msg = '''Exception raised in processing function:
{func}
Generated partial declaration:
{decl}'''.format(func=pprint.pformat(func), decl=pprint.pformat(declaration))
                print(msg, file=sys.stderr)
                raise e

    return declarations
