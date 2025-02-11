import "propertiesblocks_base.qbs" as ProductBase

Project {
    Product {
        name: "property_append"
        Depends { name: "dummy" }
        dummy.defines: ["SOMETHING"]
        Properties {
            dummy.defines: ["APPENDED"]
        }
    }
    Product {
        name: "property_set_indirect"
        Depends { name: "dummyqt.core" }
        Properties {
            dummyqt.core.zort: "VAL"
        }
    }
    Product {
        name: "property_overwrite"
        Depends { name: "dummy" }
        Properties {
            dummy.defines: ["OVERWRITTEN"]
        }
    }
    Product {
        name: "property_append_indirect"
        Depends { name: "dummy" }
        property stringList myDefines: ["ONE"]
        dummy.defines: myDefines
        Properties {
            dummy.defines: ["TWO"]
        }
    }
    ProductBase {
        name: "property_append_to_indirect_derived"
        Properties {
            dummy.cFlags: outer.concat("PROPS")
        }
    }
    ProductBase {
        name: "property_append_to_indirect_derived2"
        Properties {
            dummy.cFlags: "PROPS"
        }
        dummy.cFlags: ["PRODUCT"]
    }
    ProductBase {
        name: "property_append_to_indirect_derived3"
        Properties {
            dummy.cFlags: "PROPS"
        }
        dummy.cFlags: base.concat("PRODUCT")
    }
    Product {
        name: "property_append_to_indirect_merged"
        Depends { name: "dummy" }
        property string justOne: "ONE"
        dummy.rpaths: [justOne]
        Properties {
            dummy.rpaths: ["TWO"]
        }
    }
    Product {
        name: "multiple_exclusive_properties"
        Depends { name: "dummy" }
        Properties {
            dummy.defines: ["OVERWRITTEN"]
        }
        Properties {
            condition: false
            dummy.defines: ["IMPOSSIBLE"]
        }
        Properties {
            condition: undefined
            dummy.defines: ["SOMETHING"]
        }
    }
    Product {
        name: "multiple_exclusive_properties_no_match"
        Depends { name: "dummy" }
        Properties {
            condition: undefined
            dummy.defines: ["OVERWRITTEN"]
        }
        Properties {
            condition: false
            dummy.defines: ["IMPOSSIBLE"]
        }
    }
    Product {
        name: "multiple_exclusive_properties_append"
        Depends { name: "dummy" }
        dummy.defines: ["ONE"]
        Properties {
            dummy.defines: ["TWO"]
        }
        Properties {
            condition: false
            dummy.defines: ["IMPOSSIBLE"]
        }
    }
    Product {
        name: "ambiguous_properties"
        Depends { name: "dummy" }
        dummy.defines: ["ONE"]
        Properties {
            dummy.defines: ["TWO"]
        }
        Properties {
            condition: false
            dummy.defines: outer.concat(["IMPOSSIBLE"])
        }
        Properties {
            dummy.defines: ["THREE"]
        }
    }
    Product {
        name: "condition_refers_to_product_property"
        property bool narf: true
        property string someString: "SOMETHING"
        Depends { name: "dummy" }
        Properties {
            condition: narf
            dummy.defines: ["OVERWRITTEN"]
            someString: "OVERWRITTEN"
        }
    }
    property bool zort: true
    Product {
        name: "condition_refers_to_project_property"
        property string someString: "SOMETHING"
        Depends { name: "dummy" }
        Properties {
            condition: project.zort
            dummy.defines: ["OVERWRITTEN"]
            someString: "OVERWRITTEN"
        }
    }
    ProductBase {
        name: "inheritance_overwrite_in_subitem"
        dummy.defines: ["OVERWRITTEN_IN_SUBITEM"]
    }
    ProductBase {
        name: "inheritance_retain_base1"
        dummy.defines: base.concat("SUB")
    }
    ProductBase {
        name: "inheritance_retain_base2"
        Properties {
            condition: true
            dummy.defines: base.concat("SUB")
        }
        Properties {
            condition: undefined
            dummy.defines: ["GNAMPF"]
        }
    }
    ProductBase {
        name: "inheritance_retain_base3"
        Properties {
            condition: true
            dummy.defines: base.concat("SUB")
        }
        // no dummy.defines binding
    }
    ProductBase {
        name: "inheritance_retain_base4"
        Properties {
            condition: false
            dummy.defines: ["NEVERMORE"]
        }
        // no "else case" for dummy.defines. The value is derived from ProductBase.
    }
    ProductBase {
        name: "inheritance_condition_in_subitem1"
        defineBase: false
        dummy.defines: base.concat("SUB")
    }
    ProductBase {
        name: "inheritance_condition_in_subitem2"
        defineBase: false
        // no dummy.defines binding
    }
    Product {
        id: knolf
        name: "gnampf"
    }
    Product {
        name: "condition_references_id"
        Depends { name: "dummy" }
        Properties {
            condition: knolf.name === "gnampf"
            dummy.defines: ["OVERWRITTEN"]
        }
    }
    Product {
        name: "using_derived_Properties_item"
        Depends { name: "dummy" }
        MyProperties {
            condition: true
            dummy.defines: ["string from MyProperties"]
        }
    }
    Product {
        name: "conditional-depends"
        Depends {
            name: "dummy"
            condition: false
        }
        Properties {
            condition: false
            dummy.defines: ["a string"]
        }
    }
    Product {
        name: "use-module-with-properties-item"
        Depends { name: "module-with-properties-item" }
    }
}
