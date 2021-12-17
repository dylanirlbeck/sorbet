# typed: strict

module RBIGen::Private
  class PrivateClass; end
  class PrivateClassPulledInByClassAlias; end
  class PrivateClassPulledInByTypeTemplate; end
  class PrivateClassPulledInByPrivateMethod; end

  class PrivateClassNotReferenced; end

  class PrivateClassForTests; end
end

module RBIGen::DirectlyExported
  MyString = String
end

module RBIGen::Public
  class RefersToPrivateTypes
    extend T::Sig

    sig {params(a: RBIGen::Private::PrivateClass).void}
    def method(a)
    end

    ClassAlias = RBIGen::Private::PrivateClassPulledInByClassAlias
  end

  class FinalClass
    extend T::Helpers
    extend T::Sig
    final!

    sig(:final) {void}
    def final_method; end
  end

  class SealedClass
    extend T::Helpers
    sealed!
  end

  class AbstractClass
    extend T::Helpers
    extend T::Sig
    abstract!

    sig {abstract.returns(String)}
    def abstract_method; end
  end

  module InterfaceModule
    extend T::Helpers
    extend T::Sig
    interface!

    sig {abstract.returns(String)}
    def interface_method; end
  end

  class AbstractAndInterfaceImplementor < AbstractClass
    include InterfaceModule

    sig {override.returns(String)}
    def abstract_method
      ""
    end

    sig {override.returns(String)}
    def interface_method
      ""
    end
  end

  class MyEnum < T::Enum
    extend T::Sig
    
    enums do
      Spades = new
      Hearts = new
      Clubs = new
      Diamonds = new
    end

    sig {returns(String)}
    def to_string
      ""
    end
  end

  class MyStruct < T::Struct
    extend T::Sig
    
    prop :foo, Integer
    const :bar, T.nilable(String)
    const :quz, Float, default: 0.5
    const :singleton_type, T.class_of(MyStruct)
    const :singleton_type_with_type_params, T.class_of(ClassWithTypeParams)

    @field = T.let(10, Integer)

    sig {returns(Integer)}
    def extra_method
      10
    end
  end

  class FieldCheck
    extend T::Sig
  
    Alias = RBIGen::Public::FieldCheck
    Constant = T.let(0, Integer)
    AliasConstant = T.let(RBIGen::Public::FieldCheck, T.class_of(RBIGen::Public::FieldCheck))

    @@static_field = T.let(10, Integer)

    @statically_declared_field = T.let(0, Integer)

    sig {void}
    def initialize
      @field = T.let(0, Integer)
    end
  end

  class AliasMethod
    extend T::Sig

    alias_method :eql?, :==

    sig {params(other: BasicObject).returns(T::Boolean)}
    def ==(other)
      false
    end
  end

  class ClassWithTypeParams
    extend T::Generic
    
    A = type_template(fixed: RBIGen::Private::PrivateClassPulledInByTypeTemplate)
    B = type_template()
    C = type_member()
  end

  module ModuleWithTypeParams
    extend T::Generic

    A = type_member(:in)
    B = type_member(:out)
  end

  module VariousMethods
    extend T::Sig

    sig {params(a: RBIGen::Private::PrivateClassPulledInByPrivateMethod).void}
    private def my_method(a); end

    sig {returns(T::Array[String])}
    def returns_generic_type
      [""]
    end

    sig {void}
    private_class_method def self.kls_method; end

    sig {void}
    module_function def sample_mod_fcn; end

    def dotdotdot(...); end # error: This function does not have a `sig`
  end

  module DefDelegator
    extend T::Sig
    extend T::Helpers
    extend Forwardable

    sig {void}
    def initialize
      @field = T.let("", String)
    end

    def_delegator :@field, :length
    def_delegator :@field, :concat, :aliased_concat
    def_delegators :@field, :size, :empty
  end

  MaybeString = T.type_alias {T.nilable(String)}
  
  class AttachedClassType
    extend T::Sig

    sig {params(a: T.proc.params(arg: T.attached_class).void).void}
    def self.method(a)
    end
  end
end