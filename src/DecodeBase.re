open Relude.Globals;
open BsAbstract.Interface;

type failure = [
  | `ExpectedBoolean
  | `ExpectedString
  | `ExpectedNumber
  | `ExpectedInt
  | `ExpectedArray
  | `ExpectedObject
  | `ExpectedValidDate
  | `ExpectedValidOption
];

let failureToPartialString =
  fun
  | `ExpectedBoolean => "Expected boolean"
  | `ExpectedString => "Expected string"
  | `ExpectedNumber => "Expected number"
  | `ExpectedInt => "Expected int"
  | `ExpectedArray => "Expected array"
  | `ExpectedObject => "Expected object"
  | `ExpectedValidDate => "Expected a valid date"
  | `ExpectedValidOption => "Expected a valid option";

let failureToString = (v, json) =>
  failureToPartialString(v) ++ " but found " ++ Js.Json.stringify(json);

module type TransformError = {
  type t('a);
  let valErr: (failure, Js.Json.t) => t('a);
  let arrErr: (int, t('a)) => t('a);
  let missingFieldErr: string => t('a);
  let objErr: (string, t('a)) => t('a);
  let lazyAlt: (t('a), unit => t('a)) => t('a);
};

module DecodeBase = (T: TransformError, M: MONAD with type t('a) = T.t('a)) => {
  module InnerApply = Relude.Extensions.Apply.ApplyExtensions(M);

  module Functor: FUNCTOR with type t('a) = Js.Json.t => M.t('a) = {
    type t('a) = Js.Json.t => M.t('a);
    let map = (f, decode) => decode >> M.map(f);
  };

  module Apply: APPLY with type t('a) = Js.Json.t => M.t('a) = {
    include Functor;
    let apply = (f, decode, json) => M.apply(f(json), decode(json));
  };

  module Applicative: APPLICATIVE with type t('a) = Js.Json.t => M.t('a) = {
    include Apply;
    let pure = (v, _) => M.pure(v);
  };

  module Monad: MONAD with type t('a) = Js.Json.t => M.t('a) = {
    include Applicative;
    let flat_map = (decode, f, json) =>
      M.flat_map(decode(json), f(_, json));
  };

  module Alt: ALT with type t('a) = Js.Json.t => M.t('a) = {
    include Functor;
    let alt = (a, b, json) => T.lazyAlt(a(json), () => b(json));
  };

  let map = Functor.map;
  let apply = Apply.apply;
  let pure = Applicative.pure;
  let flatMap = (f, decode) => Monad.flat_map(decode, f);
  let alt = Alt.alt;
  include Relude.Extensions.Apply.ApplyExtensions(Apply);

  let value = (decode, failure, json) =>
    decode(json) |> Option.foldLazy(() => T.valErr(failure, json), M.pure);

  let boolean = value(Js.Json.decodeBoolean, `ExpectedBoolean);

  let string = value(Js.Json.decodeString, `ExpectedString);

  let floatFromNumber = value(Js.Json.decodeNumber, `ExpectedNumber);

  [@ocaml.deprecated "Use floatFromNumber instead."]
  let float = floatFromNumber;

  let intFromNumber = {
    let isInt = v => v == 0.0 || mod_float(v, floor(v)) == 0.0;
    flatMap(
      v => isInt(v) ? pure(int_of_float(v)) : T.valErr(`ExpectedInt),
      floatFromNumber,
    );
  };

  [@ocaml.deprecated "Use intFromNumber instead."]
  let int = intFromNumber;

  let date = {
    let fromFloat = map(Js.Date.fromFloat, floatFromNumber);
    let fromString = map(Js.Date.fromString, string);
    let isValid = date =>
      date
      |> Js.Date.toJSONUnsafe
      |> Js.Nullable.return
      |> Js.Nullable.isNullable
        ? T.valErr(`ExpectedValidDate) : pure(date);

    alt(fromFloat, fromString) |> flatMap(isValid);
  };

  let variantFromJson = (jsonToJs, jsToVariant) =>
    jsonToJs
    |> map(jsToVariant)
    |> flatMap(Option.foldLazy(() => T.valErr(`ExpectedValidOption), pure));

  let variantFromString = (stringToVariant, json) =>
    variantFromJson(string, stringToVariant, json);

  let variantFromInt = (intToVariant, json) =>
    variantFromJson(intFromNumber, intToVariant, json);

  let optional = (decode, json) =>
    switch (Js.Json.decodeNull(json)) {
    | Some(_) => pure(None, json)
    | None => map(v => Some(v), decode, json)
    };

  let array = decode => {
    let map2 = (f, a, b) => M.map(f, a) |> M.apply(_, b);
    let decodeEach = (arr, _json) =>
      Array.foldLeft(
        ((pos, acc), curr) => {
          let decoded = T.arrErr(pos, decode(curr));
          let result = map2(flip(Array.append), acc, decoded);
          (pos + 1, result);
        },
        (0, M.pure([||])),
        arr,
      )
      |> snd;

    value(Js.Json.decodeArray, `ExpectedArray) |> flatMap(decodeEach);
  };

  let list = decode => array(decode) |> map(Array.toList);

  let dict = decode => {
    let rec decodeEntries =
      fun
      | [] => pure([])
      | [(key, value), ...xs] =>
        map2(
          (decodedValue, rest) => [(key, decodedValue), ...rest],
          _ => T.objErr(key, decode(value)),
          decodeEntries(xs),
        );

    value(Js.Json.decodeObject, `ExpectedObject)
    |> map(Js.Dict.entries >> Array.toList)
    |> flatMap(decodeEntries)
    |> map(Js.Dict.fromList);
  };

  let rec at = (fields, decode) =>
    switch (fields) {
    | [] => decode
    | [x, ...xs] =>
      value(Js.Json.decodeObject, `ExpectedObject)
      |> map(Js.Dict.get(_, x))
      |> flatMap(Option.fold(_ => T.missingFieldErr(x), pure))
      |> flatMap(at(xs, decode) >> T.objErr(x) >> const)
    };

  let field = (name, decode) => at([name], decode);

  let optionalField = (name, decode) =>
    value(Js.Json.decodeObject, `ExpectedObject)
    |> map(Js.Dict.get(_, name))
    |> flatMap(
         fun
         | None => pure(None)
         | Some(v) => (_ => optional(decode, v)),
       );

  let fallback = (decode, recovery) => alt(decode, pure(recovery));

  let tuple = ((fieldA, decodeA), (fieldB, decodeB)) =>
    map2(
      (a, b) => (a, b),
      field(fieldA, decodeA),
      field(fieldB, decodeB),
    );

  let oneOf = (decode, rest) => List.foldLeft(alt, decode, rest);

  module Pipeline = {
    /**
     * `succeed` returns a `json => Result` decode function that ignores the `json`
     * argument and always returns `Ok`
     */
    let succeed = pure;

    let pipe = (a, b, json) => map2((|>), a, b, json);

    let optionalField = (name, decode) => pipe(optionalField(name, decode));

    let fallbackField = (name, decode, recovery) =>
      pipe(fallback(field(name, decode), recovery));

    let field = (name, decode) => pipe(field(name, decode));

    let at = (fields, decode) => pipe(at(fields, decode));

    let hardcoded = v => pipe(pure(v));

    /**
     * `run` takes a decoder and some json, and it passes that json to the
     * decoder. The result is that your decoder is run with the provided json
     */
    let run = (json, decode) => decode(json);

    /**
     * Alias many functions from outside the Pipeline for easy local opens
     */
    let map = map;
    let apply = apply;
    let map2 = map2;
    let map3 = map3;
    let map4 = map4;
    let map5 = map5;
    let pure = pure;
    let flatMap = flatMap;
    let boolean = boolean;
    let string = string;
    let floatFromNumber = floatFromNumber;
    let intFromNumber = intFromNumber;
    let intFromNumber = intFromNumber;
    let date = date;
    let variantFromJson = variantFromJson;
    let variantFromString = variantFromString;
    let variantFromInt = variantFromInt;
    let optional = optional;
    let array = array;
    let list = list;
    let tuple = tuple;
    let dict = dict;
    let oneOf = oneOf;
    let fallback = fallback;
  };
};
